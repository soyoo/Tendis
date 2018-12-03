#include <string>
#include <utility>
#include <memory>
#include <algorithm>
#include <cctype>
#include <clocale>
#include <vector>
#include <list>
#include "glog/logging.h"
#include "tendisplus/utils/sync_point.h"
#include "tendisplus/utils/string.h"
#include "tendisplus/utils/invariant.h"
#include "tendisplus/utils/redis_port.h"
#include "tendisplus/commands/command.h"

namespace tendisplus {

Expected<std::string> hincrfloatGeneric(const RecordKey& metaRk,
                   const RecordKey& subRk,
                   long double inc,
                   PStore kvstore) {
    auto ptxn = kvstore->createTransaction();
    if (!ptxn.ok()) {
        return ptxn.status();
    }
    std::unique_ptr<Transaction> txn = std::move(ptxn.value());
    Expected<RecordValue> eValue = kvstore->getKV(metaRk, txn.get());
    if (!eValue.ok() && eValue.status().code() != ErrorCodes::ERR_NOTFOUND) {
        return eValue.status();
    }

    HashMetaValue hashMeta;
    uint64_t ttl = 0;
    if (eValue.ok()) {
        ttl = eValue.value().getTtl();
        Expected<HashMetaValue> exptHashMeta =
            HashMetaValue::decode(eValue.value().getValue());
        if (!exptHashMeta.ok()) {
            return exptHashMeta.status();
        }
        hashMeta = std::move(exptHashMeta.value());
    }  // no else, else not found , so subkeyCount = 0, ttl = 0

    auto getSubkeyExpt = kvstore->getKV(subRk, txn.get());
    long double nowVal = 0;
    if (getSubkeyExpt.ok()) {
        Expected<long double> val =
            ::tendisplus::stold(getSubkeyExpt.value().getValue());
        if (!val.ok()) {
            return {ErrorCodes::ERR_DECODE, "hash value is not a valid float"};
        }
        nowVal = val.value();
    } else if (getSubkeyExpt.status().code() == ErrorCodes::ERR_NOTFOUND) {
        nowVal = 0;
        hashMeta.setCount(hashMeta.getCount()+1);
    } else {
        return getSubkeyExpt.status();
    }

    nowVal += inc;
    RecordValue newVal(redis_port::ldtos(nowVal));
    RecordValue metaValue(hashMeta.encode(), ttl);
    Status setStatus = kvstore->setKV(metaRk, metaValue, txn.get());
    if (!setStatus.ok()) {
        return setStatus;
    }
    setStatus = kvstore->setKV(subRk, newVal, txn.get());
    if (!setStatus.ok()) {
        return setStatus;
    }
    Expected<uint64_t> exptCommit = txn->commit();
    if (!exptCommit.ok()) {
        return exptCommit.status();
    } else {
        return Command::fmtBulk(redis_port::ldtos(nowVal));
    }
}

Expected<std::string> hincrGeneric(const RecordKey& metaRk,
                   const RecordKey& subRk,
                   int64_t inc,
                   PStore kvstore) {
    auto ptxn = kvstore->createTransaction();
    if (!ptxn.ok()) {
        return ptxn.status();
    }
    std::unique_ptr<Transaction> txn = std::move(ptxn.value());
    Expected<RecordValue> eValue = kvstore->getKV(metaRk, txn.get());
    if (!eValue.ok() && eValue.status().code() != ErrorCodes::ERR_NOTFOUND) {
        return eValue.status();
    }

    HashMetaValue hashMeta;
    uint64_t ttl = 0;
    if (eValue.ok()) {
        ttl = eValue.value().getTtl();
        Expected<HashMetaValue> exptHashMeta =
            HashMetaValue::decode(eValue.value().getValue());
        if (!exptHashMeta.ok()) {
            return exptHashMeta.status();
        }
        hashMeta = std::move(exptHashMeta.value());
    }  // no else, else not found , so subkeyCount = 0, ttl = 0

    auto getSubkeyExpt = kvstore->getKV(subRk, txn.get());
    int64_t nowVal = 0;
    if (getSubkeyExpt.ok()) {
        Expected<int64_t> val =
            ::tendisplus::stoll(getSubkeyExpt.value().getValue());
        if (!val.ok()) {
            return {ErrorCodes::ERR_DECODE, "hash value is not an integer "};
        }
        nowVal = val.value();
    } else if (getSubkeyExpt.status().code() == ErrorCodes::ERR_NOTFOUND) {
        nowVal = 0;
        hashMeta.setCount(hashMeta.getCount()+1);
    } else {
        return getSubkeyExpt.status();
    }

    if ((inc < 0 && nowVal < 0 && inc < (LLONG_MIN - nowVal)) ||
            (inc > 0 && nowVal > 0 && inc > (LLONG_MAX - nowVal))) {
        return {ErrorCodes::ERR_OVERFLOW,
                    "increment or decrement would overflow"};
    }
    nowVal += inc;
    RecordValue newVal(std::to_string(nowVal));
    RecordValue metaValue(hashMeta.encode(), ttl);
    Status setStatus = kvstore->setKV(metaRk, metaValue, txn.get());
    if (!setStatus.ok()) {
        return setStatus;
    }
    setStatus = kvstore->setKV(subRk, newVal, txn.get());
    if (!setStatus.ok()) {
        return setStatus;
    }
    Expected<uint64_t> exptCommit = txn->commit();
    if (!exptCommit.ok()) {
        return exptCommit.status();
    } else {
        return Command::fmtLongLong(nowVal);
    }
}

class HLenCommand: public Command {
 public:
    HLenCommand()
        :Command("hlen") {
    }

    ssize_t arity() const {
        return 2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);

        RecordKey metaRk(pCtx->getDbId(), RecordType::RT_HASH_META, key, "");
        std::string metaKeyEnc = metaRk.encode();
        uint32_t storeId = Command::getStoreId(sess, key);

        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, storeId, metaRk);
        if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
            return fmtZero();
        } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return fmtZero();
        } else if (!rv.status().ok()) {
            return rv.status();
        }
        Expected<HashMetaValue> exptHashMeta =
            HashMetaValue::decode(rv.value().getValue());
        if (!exptHashMeta.ok()) {
            return exptHashMeta.status();
        }
        return fmtLongLong(exptHashMeta.value().getCount());
    }
} hlenCommand;

class HExistsCommand: public Command {
 public:
    HExistsCommand()
        :Command("hexists") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];
        const std::string& subkey = args[2];

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);

        RecordKey metaRk(pCtx->getDbId(), RecordType::RT_HASH_META, key, "");
        std::string metaKeyEnc = metaRk.encode();
        RecordKey subRk(pCtx->getDbId(), RecordType::RT_HASH_ELE, key, subkey);
        uint32_t storeId = Command::getStoreId(sess, key);

        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, storeId, metaRk);
        if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
            return Command::fmtNull();
        } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return Command::fmtNull();
        } else if (!rv.status().ok()) {
            return rv.status();
        }

        auto storeLock = Command::lockDBByKey(sess,
                                              key,
                                              mgl::LockMode::LOCK_IS);
        if (Command::isKeyLocked(sess, storeId, metaKeyEnc)) {
            return {ErrorCodes::ERR_BUSY, "key locked"};
        }

        PStore kvstore = Command::getStoreById(sess, storeId);
        auto ptxn = kvstore->createTransaction();
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());
        Expected<RecordValue> eVal = kvstore->getKV(subRk, txn.get());
        if (eVal.ok()) {
            return Command::fmtOne();
        } else if (eVal.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return Command::fmtZero();
        } else {
            return eVal.status();
        }
    }
} hexistsCmd;

class HAllCommand: public Command {
 public:
    explicit HAllCommand(const std::string& name)
        :Command(name) {
    }

    ssize_t arity() const {
        return 2;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::list<Record>> getRecords(Session *sess) {
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);

        RecordKey metaRk(pCtx->getDbId(), RecordType::RT_HASH_META, key, "");
        std::string metaKeyEnc = metaRk.encode();
        uint32_t storeId = Command::getStoreId(sess, key);

        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, storeId, metaRk);
        if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
            return std::list<Record>();
        } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return std::list<Record>();
        } else if (!rv.status().ok()) {
            return rv.status();
        }

        auto storeLock = Command::lockDBByKey(sess,
                                              key,
                                              mgl::LockMode::LOCK_IS);
        if (Command::isKeyLocked(sess, storeId, metaKeyEnc)) {
            return {ErrorCodes::ERR_BUSY, "key locked"};
        }

        PStore kvstore = Command::getStoreById(sess, storeId);
        auto ptxn = kvstore->createTransaction();
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());
        RecordKey fakeEle(metaRk.getDbId(),
                          RecordType::RT_HASH_ELE,
                          metaRk.getPrimaryKey(),
                          "");
        std::string prefix = fakeEle.prefixPk();
        auto cursor = txn->createCursor();
        cursor->seek(prefix);

        std::list<Record> result;
        while (true) {
            Expected<Record> exptRcd = cursor->next();
            if (exptRcd.status().code() == ErrorCodes::ERR_EXHAUST) {
                break;
            }
            if (!exptRcd.ok()) {
                return exptRcd.status();
            }
            Record& rcd = exptRcd.value();
            const RecordKey& rcdKey = rcd.getRecordKey();
            if (rcdKey.prefixPk() != prefix) {
                INVARIANT(rcdKey.getPrimaryKey() != metaRk.getPrimaryKey());
                break;
            }
            result.emplace_back(std::move(rcd));
        }
        return std::move(result);
    }
};

class HGetAllCommand: public HAllCommand {
 public:
    HGetAllCommand()
        :HAllCommand("hgetall") {
    }

    Expected<std::string> run(Session *sess) final {
        Expected<std::list<Record>> rcds = getRecords(sess);
        if (!rcds.ok()) {
            return rcds.status();
        }
        std::stringstream ss;
        Command::fmtMultiBulkLen(ss, rcds.value().size()*2);
        for (const auto& v : rcds.value()) {
            Command::fmtBulk(ss, v.getRecordKey().getSecondaryKey());
            Command::fmtBulk(ss, v.getRecordValue().getValue());
        }
        return ss.str();
    }
} hgetAllCmd;

class HKeysCommand: public HAllCommand {
 public:
    HKeysCommand()
        :HAllCommand("hkeys") {
    }

    Expected<std::string> run(Session *sess) final {
        Expected<std::list<Record>> rcds = getRecords(sess);
        if (!rcds.ok()) {
            return rcds.status();
        }
        std::stringstream ss;
        Command::fmtMultiBulkLen(ss, rcds.value().size());
        for (const auto& v : rcds.value()) {
            Command::fmtBulk(ss, v.getRecordKey().getSecondaryKey());
        }
        return ss.str();
    }
} hkeysCmd;

class HValsCommand: public HAllCommand {
 public:
    HValsCommand()
        :HAllCommand("hvals") {
    }

    Expected<std::string> run(Session *sess) final {
        Expected<std::list<Record>> rcds = getRecords(sess);
        if (!rcds.ok()) {
            return rcds.status();
        }
        std::stringstream ss;
        Command::fmtMultiBulkLen(ss, rcds.value().size());
        for (const auto& v : rcds.value()) {
            Command::fmtBulk(ss, v.getRecordValue().getValue());
        }
        return ss.str();
    }
} hvalsCmd;

class HGetRecordCommand: public Command {
 public:
    explicit HGetRecordCommand(const std::string& name)
        :Command(name) {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<Record> getRecord(Session *sess) {
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];
        const std::string& subkey = args[2];

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);

        RecordKey metaRk(pCtx->getDbId(), RecordType::RT_HASH_META, key, "");
        std::string metaKeyEnc = metaRk.encode();
        RecordKey subRk(pCtx->getDbId(), RecordType::RT_HASH_ELE, key, subkey);
        uint32_t storeId = Command::getStoreId(sess, key);

        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, storeId, metaRk);
        if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
            return rv.status();
        } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return rv.status();
        } else if (!rv.status().ok()) {
            return rv.status();
        }

        auto storeLock = Command::lockDBByKey(sess,
                                              key,
                                              mgl::LockMode::LOCK_IS);
        if (Command::isKeyLocked(sess, storeId, metaKeyEnc)) {
            return {ErrorCodes::ERR_BUSY, "key locked"};
        }

        PStore kvstore = Command::getStoreById(sess, storeId);
        auto ptxn = kvstore->createTransaction();
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());
        Expected<RecordValue> eVal = kvstore->getKV(subRk, txn.get());
        if (eVal.ok()) {
            return std::move(Record(std::move(subRk), std::move(eVal.value())));
        } else {
            return eVal.status();
        }
    }
};

class HGetCommand: public HGetRecordCommand {
 public:
    HGetCommand()
        :HGetRecordCommand("hget") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        Expected<Record> ercd = getRecord(sess);
        if (ercd.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return Command::fmtNull();
        } else if (ercd.status().code() == ErrorCodes::ERR_EXPIRED) {
            return Command::fmtNull();
        } else if (!ercd.ok()) {
            return ercd.status();
        }
        return Command::fmtBulk(ercd.value().getRecordValue().getValue());
    }
} hgetCommand;

class HStrlenCommand: public HGetRecordCommand {
 public:
    HStrlenCommand()
        :HGetRecordCommand("hstrlen") {
    }

    ssize_t arity() const {
        return 3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        Expected<Record> ercd = getRecord(sess);
        if (ercd.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return Command::fmtZero();
        } else if (ercd.status().code() == ErrorCodes::ERR_EXPIRED) {
            return Command::fmtZero();
        } else if (!ercd.ok()) {
            return ercd.status();
        }
        uint64_t size = ercd.value().getRecordValue().getValue().size();
        return Command::fmtLongLong(size);
    }
} hstrlenCommand;

class HIncrByFloatCommand: public Command {
 public:
    HIncrByFloatCommand()
        :Command("hincrbyfloat") {
    }

    ssize_t arity() const {
        return 4;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];
        const std::string& subkey = args[2];
        const std::string& val = args[3];
        Expected<long double> inc = ::tendisplus::stold(val);
        if (!inc.ok()) {
            return inc.status();
        }

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        RecordKey metaKey(pCtx->getDbId(), RecordType::RT_HASH_META, key, "");
        std::string metaKeyEnc = metaKey.encode();
        RecordKey subKey(pCtx->getDbId(), RecordType::RT_HASH_ELE, key, subkey);
        uint32_t storeId = Command::getStoreId(sess, key);

        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, storeId, metaKey);
        if (rv.status().code() != ErrorCodes::ERR_OK &&
                rv.status().code() != ErrorCodes::ERR_EXPIRED &&
                rv.status().code() != ErrorCodes::ERR_NOTFOUND) {
            return rv.status();
        }

        // now, we have no need to deal with expire, though it may still
        // be expired in a very rare situation since expireHash is in
        // a seperate txn (from code below)
        auto storeLock = Command::lockDBByKey(sess,
                                              key,
                                              mgl::LockMode::LOCK_IX);
        if (Command::isKeyLocked(sess, storeId, metaKeyEnc)) {
            return {ErrorCodes::ERR_BUSY, "key locked"};
        }
        PStore kvstore = Command::getStoreById(sess, storeId);

        // here maybe one more time io than the original tendis
        for (int32_t i = 0; i < RETRY_CNT - 1; ++i) {
            auto result =
                hincrfloatGeneric(metaKey, subKey, inc.value(), kvstore);
            if (result.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return result;
            }
        }
        return hincrfloatGeneric(metaKey, subKey, inc.value(), kvstore);
    }
} hincrbyfloatCmd;

class HIncrByCommand: public Command {
 public:
    HIncrByCommand()
        :Command("hincrby") {
    }

    ssize_t arity() const {
        return 4;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];
        const std::string& subkey = args[2];
        const std::string& val = args[3];
        Expected<int64_t> inc = ::tendisplus::stoll(val);
        if (!inc.ok()) {
            return inc.status();
        }

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        RecordKey metaKey(pCtx->getDbId(), RecordType::RT_HASH_META, key, "");
        std::string metaKeyEnc = metaKey.encode();
        RecordKey subKey(pCtx->getDbId(), RecordType::RT_HASH_ELE, key, subkey);
        uint32_t storeId = Command::getStoreId(sess, key);

        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, storeId, metaKey);
        if (rv.status().code() != ErrorCodes::ERR_OK &&
                rv.status().code() != ErrorCodes::ERR_EXPIRED &&
                rv.status().code() != ErrorCodes::ERR_NOTFOUND) {
            return rv.status();
        }

        // now, we have no need to deal with expire, though it may still
        // be expired in a very rare situation since expireHash is in
        // a seperate txn (from code below)
        auto storeLock = Command::lockDBByKey(sess,
                                              key,
                                              mgl::LockMode::LOCK_IX);
        if (Command::isKeyLocked(sess, storeId, metaKeyEnc)) {
            return {ErrorCodes::ERR_BUSY, "key locked"};
        }
        PStore kvstore = Command::getStoreById(sess, storeId);

        // here maybe one more time io than the original tendis
        for (int32_t i = 0; i < RETRY_CNT - 1; ++i) {
            auto result = hincrGeneric(metaKey, subKey, inc.value(), kvstore);
            if (result.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return result;
            }
        }
        return hincrGeneric(metaKey, subKey, inc.value(), kvstore);
    }
} hincrbyCommand;

class HMGetCommand: public Command {
 public:
    HMGetCommand()
        :Command("hmget") {
    }

    ssize_t arity() const {
        return -3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        RecordKey metaKey(pCtx->getDbId(), RecordType::RT_HASH_META, key, "");
        std::string metaKeyEnc = metaKey.encode();
        uint32_t storeId = Command::getStoreId(sess, key);

        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, storeId, metaKey);
        if (rv.status().code() != ErrorCodes::ERR_OK &&
                rv.status().code() != ErrorCodes::ERR_EXPIRED &&
                rv.status().code() != ErrorCodes::ERR_NOTFOUND) {
            return rv.status();
        }

        auto storeLock = Command::lockDBByKey(sess,
                                              key,
                                              mgl::LockMode::LOCK_IS);
        if (Command::isKeyLocked(sess, storeId, metaKeyEnc)) {
            return {ErrorCodes::ERR_BUSY, "key locked"};
        }
        PStore kvstore = Command::getStoreById(sess, storeId);
        auto ptxn = kvstore->createTransaction();
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());
 
        std::stringstream ss;
        Command::fmtMultiBulkLen(ss, args.size()-2);
        for (size_t i = 2; i < args.size(); ++i) {
            RecordKey subKey(pCtx->getDbId(), RecordType::RT_HASH_ELE, key, args[i]);
            Expected<RecordValue> eValue = kvstore->getKV(subKey, txn.get());
            if (!eValue.ok()) {
                if (eValue.status().code() == ErrorCodes::ERR_NOTFOUND) {
                    Command::fmtNull(ss);
                } else {
                    return eValue.status();
                }
            } else {
                Command::fmtBulk(ss, eValue.value().getValue());
            }
        }
        return ss.str();
    }
} hmgetCmd;

class HMSetCommand: public Command {
 public:
    HMSetCommand()
        :Command("hmset") {
    }

    ssize_t arity() const {
        return -4;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> hmsetGeneric(const RecordKey& metaRk,
                        const std::vector<Record>& rcds,
                        PStore kvstore) {
        auto ptxn = kvstore->createTransaction();
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());
        Expected<RecordValue> eValue = kvstore->getKV(metaRk, txn.get());
        if (!eValue.ok()
                && eValue.status().code() != ErrorCodes::ERR_NOTFOUND) {
            return eValue.status();
        }

        HashMetaValue hashMeta;
        uint64_t ttl = 0;
        uint32_t inserted = 0;
        if (eValue.ok()) {
            ttl = eValue.value().getTtl();
            Expected<HashMetaValue> exptHashMeta =
                HashMetaValue::decode(eValue.value().getValue());
            if (!exptHashMeta.ok()) {
                return exptHashMeta.status();
            }
            hashMeta = std::move(exptHashMeta.value());
        }  // no else, else not found , so subkeyCount = 0, ttl = 0

        for (const auto& v : rcds) {
            auto getSubkeyExpt = kvstore->getKV(v.getRecordKey(), txn.get());
            if (!getSubkeyExpt.ok()) {
                if (getSubkeyExpt.status().code() != ErrorCodes::ERR_NOTFOUND) {
                    return getSubkeyExpt.status();
                }
                inserted += 1;
            }
            Status setStatus = kvstore->setKV(v.getRecordKey(), v.getRecordValue(), txn.get());
            if (!setStatus.ok()) {
                return setStatus;
            }
        }
        hashMeta.setCount(hashMeta.getCount() + inserted);
        RecordValue metaValue(hashMeta.encode(), ttl);
        Status setStatus = kvstore->setKV(metaRk, metaValue, txn.get());
        if (!setStatus.ok()) {
            return setStatus;
        }
        Expected<uint64_t> exptCommit = txn->commit();
        if (!exptCommit.ok()) {
            return exptCommit.status();
        } else {
            return Command::fmtOK();
        }
    }

    Expected<std::string> run(Session *sess) final {
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];
        if (args.size()-2 > 2048) {
            std::stringstream ss;
            ss << "exceed batch lim:" << args.size();
            return {ErrorCodes::ERR_INTERNAL, ss.str()};
        }
        if (args.size() % 2 != 0) {
            return {ErrorCodes::ERR_PARSEOPT, "invalid args size"};
        }

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        RecordKey metaKey(pCtx->getDbId(), RecordType::RT_HASH_META, key, "");
        std::string metaKeyEnc = metaKey.encode();
        uint32_t storeId = Command::getStoreId(sess, key);

        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, storeId, metaKey);
        if (rv.status().code() != ErrorCodes::ERR_OK &&
                rv.status().code() != ErrorCodes::ERR_EXPIRED &&
                rv.status().code() != ErrorCodes::ERR_NOTFOUND) {
            return rv.status();
        }

        auto storeLock = Command::lockDBByKey(sess,
                                              key,
                                              mgl::LockMode::LOCK_IX);
        if (Command::isKeyLocked(sess, storeId, metaKeyEnc)) {
            return {ErrorCodes::ERR_BUSY, "key locked"};
        }
        PStore kvstore = Command::getStoreById(sess, storeId);
        std::vector<Record> rcds;
        for (size_t i = 2; i < args.size(); i+=2) {
            RecordKey subKey(pCtx->getDbId(), RecordType::RT_HASH_ELE, key, args[i]);
            RecordValue subRv(args[i+1]);
            rcds.emplace_back(Record(std::move(subKey), std::move(subRv)));
        }
        for (int32_t i = 0; i < RETRY_CNT - 1; ++i) {
            auto result = hmsetGeneric(metaKey, rcds, kvstore);
            if (result.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return result;
            }
        }
        return hmsetGeneric(metaKey, rcds, kvstore);
    }
} hmsetCommand;

class HSetGeneric: public Command {
 public:
    HSetGeneric(const std::string& name, bool setNx)
            :Command(name) {
        _setNx = setNx;
    }

    ssize_t arity() const {
        return 4;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];
        const std::string& subkey = args[2];
        const std::string& val = args[3];

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        RecordKey metaKey(pCtx->getDbId(), RecordType::RT_HASH_META, key, "");
        std::string metaKeyEnc = metaKey.encode();
        RecordKey subKey(pCtx->getDbId(), RecordType::RT_HASH_ELE, key, subkey);
        RecordValue subRv(val);
        uint32_t storeId = Command::getStoreId(sess, key);

        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, storeId, metaKey);
        if (rv.status().code() != ErrorCodes::ERR_OK &&
                rv.status().code() != ErrorCodes::ERR_EXPIRED &&
                rv.status().code() != ErrorCodes::ERR_NOTFOUND) {
            return rv.status();
        }

        // now, we have no need to deal with expire, though it may still
        // be expired in a very rare situation since expireHash is in
        // a seperate txn (from code below)
        auto storeLock = Command::lockDBByKey(sess,
                                              key,
                                              mgl::LockMode::LOCK_IX);
        if (Command::isKeyLocked(sess, storeId, metaKeyEnc)) {
            return {ErrorCodes::ERR_BUSY, "key locked"};
        }
        PStore kvstore = Command::getStoreById(sess, storeId);

        // here maybe one more time io than the original tendis
        for (int32_t i = 0; i < RETRY_CNT - 1; ++i) {
            auto result = hsetGeneric(metaKey, subKey, subRv, kvstore);
            if (result.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return result;
            }
        }
        return hsetGeneric(metaKey, subKey, subRv, kvstore);
    }

    Expected<std::string> hsetGeneric(const RecordKey& metaRk,
                       const RecordKey& subRk,
                       const RecordValue& subRv,
                       PStore kvstore) {
        auto ptxn = kvstore->createTransaction();
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());
        Expected<RecordValue> eValue = kvstore->getKV(metaRk, txn.get());
        if (!eValue.ok()
                && eValue.status().code() != ErrorCodes::ERR_NOTFOUND) {
            return eValue.status();
        }

        HashMetaValue hashMeta;
        uint64_t ttl = 0;
        if (eValue.ok()) {
            ttl = eValue.value().getTtl();
            Expected<HashMetaValue> exptHashMeta =
                HashMetaValue::decode(eValue.value().getValue());
            if (!exptHashMeta.ok()) {
                return exptHashMeta.status();
            }
            hashMeta = std::move(exptHashMeta.value());
        }  // no else, else not found , so subkeyCount = 0, ttl = 0

        bool updated = false;
        auto getSubkeyExpt = kvstore->getKV(subRk, txn.get());
        if (getSubkeyExpt.ok()) {
            updated = true;
        } else if (getSubkeyExpt.status().code() == ErrorCodes::ERR_NOTFOUND) {
            updated = false;
            hashMeta.setCount(hashMeta.getCount()+1);
        } else {
            return getSubkeyExpt.status();
        }

        if (_setNx && updated) {
            return Command::fmtZero();
        }

        RecordValue metaValue(hashMeta.encode(), ttl);
        Status setStatus = kvstore->setKV(metaRk, metaValue, txn.get());
        if (!setStatus.ok()) {
            return setStatus;
        }
        setStatus = kvstore->setKV(subRk, subRv, txn.get());
        if (!setStatus.ok()) {
            return setStatus;
        }
        Expected<uint64_t> exptCommit = txn->commit();
        if (!exptCommit.ok()) {
            return exptCommit.status();
        } else {
            return updated ? Command::fmtZero() : Command::fmtOne();
        }
    }

 private:
    bool _setNx;
};

class HSetCommand: public HSetGeneric {
 public:
    HSetCommand()
        :HSetGeneric("hset", false) {
    }
} hsetCommand;

class HSetNxCommand: public HSetGeneric {
 public:
    HSetNxCommand()
        :HSetGeneric("hsetnx", true) {
    }
} hsetNxCommand;

class HDelCommand: public Command {
 public:
    HDelCommand()
        :Command("hdel") {
    }

    ssize_t arity() const {
        return -3;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<uint32_t> delKeys(PStore kvstore,
                               const RecordKey& metaKey,
                               const std::vector<std::string>& args,
                               Transaction* txn) {
        uint32_t dbId = metaKey.getDbId();
        uint32_t realDel = 0;

        Expected<RecordValue> eValue = kvstore->getKV(metaKey, txn);
        if (eValue.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return 0;
        }
        if (!eValue.ok()) {
            return eValue.status();
        }

        HashMetaValue hashMeta;
        uint64_t ttl = 0;
        if (eValue.ok()) {
            ttl = eValue.value().getTtl();
            Expected<HashMetaValue> exptHashMeta =
                HashMetaValue::decode(eValue.value().getValue());
            if (!exptHashMeta.ok()) {
                return exptHashMeta.status();
            }
            hashMeta = std::move(exptHashMeta.value());
        }  // no else, else not found , so subkeyCount = 0, ttl = 0

        for (size_t i = 2; i < args.size(); ++i) {
            RecordKey subRk(dbId,
                            RecordType::RT_HASH_ELE,
                            metaKey.getPrimaryKey(),
                            args[i]);
            Expected<RecordValue> eVal = kvstore->getKV(subRk, txn);
            if (eVal.status().code() == ErrorCodes::ERR_NOTFOUND) {
                continue;
            }
            if (!eVal.ok()) {
                return eVal.status();
            }
            Status s = kvstore->delKV(subRk, txn);
            if (!s.ok()) {
                return s;
            }
            realDel++;
        }

        // modify meta data
        INVARIANT(realDel <= hashMeta.getCount());
        Status s;
        if (realDel == hashMeta.getCount()) {
            s = kvstore->delKV(metaKey, txn);
        } else {
            hashMeta.setCount(hashMeta.getCount() - realDel);
            RecordValue metaValue(hashMeta.encode(), ttl);
            s = kvstore->setKV(metaKey, metaValue, txn);
        }
        if (!s.ok()) {
            return s;
        }
        Expected<uint64_t> commitStatus = txn->commit();
        if (!commitStatus.ok()) {
            return commitStatus.status();
        }
        return realDel;
    }

    Expected<std::string> run(Session* sess) final {
        const std::vector<std::string>& args = sess->getArgs();
        const std::string& key = args[1];
        if (args.size() >= 30000) {
            return {ErrorCodes::ERR_PARSEOPT, "exceed hdel batch lim"};
        }

        SessionCtx *pCtx = sess->getCtx();
        INVARIANT(pCtx != nullptr);
        uint32_t storeId = Command::getStoreId(sess, key);
        RecordKey metaKey(pCtx->getDbId(), RecordType::RT_HASH_META, key, "");
        std::string metaKeyEnc = metaKey.encode();

        Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, storeId, metaKey);
        if (rv.status().code() == ErrorCodes::ERR_EXPIRED) {
            return fmtZero();
        } else if (rv.status().code() == ErrorCodes::ERR_NOTFOUND) {
            return fmtZero();
        } else if (!rv.status().ok()) {
            return rv.status();
        }
        auto storeLock = Command::lockDBByKey(sess,
                                              key,
                                              mgl::LockMode::LOCK_IX);
        if (Command::isKeyLocked(sess, storeId, metaKeyEnc)) {
            return {ErrorCodes::ERR_BUSY, "key locked"};
        }

        std::vector<RecordKey> rcds;
        for (size_t i = 2; i < args.size(); ++i) {
            rcds.emplace_back(
                RecordKey(pCtx->getDbId(),
                          RecordType::RT_HASH_ELE,
                          key,
                          args[i]));
        }

        PStore kvstore = Command::getStoreById(sess, storeId);
        for (uint32_t i = 0; i < RETRY_CNT; ++i) {
            auto ptxn = kvstore->createTransaction();
            if (!ptxn.ok()) {
                return ptxn.status();
            }
            std::unique_ptr<Transaction> txn = std::move(ptxn.value());
            Expected<uint32_t> delCount =
                    delKeys(kvstore, metaKey, args, txn.get());
            if (delCount.status().code() == ErrorCodes::ERR_COMMIT_RETRY) {
                if (i == RETRY_CNT - 1) {
                    return delCount.status();
                } else {
                    continue;
                }
            }
            if (!delCount.ok()) {
                return delCount.status();
            }
            return Command::fmtLongLong(delCount.value());
        }
        // never reaches here
        INVARIANT(0);
        return {ErrorCodes::ERR_INTERNAL, "never reaches here"};
    }
} hdelCommand;

}  // namespace tendisplus
