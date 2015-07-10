/* Copyright (c) 2015 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TestUtil.h"       //Has to be first, compiler complains
#include "MockCluster.h"
#include "ClientTransactionTask.h"

namespace RAMCloud {

class ClientTransactionTaskTest : public ::testing::Test {
  public:
    TestLog::Enable logEnabler;
    Context context;
    MockCluster cluster;
    Tub<RamCloud> ramcloud;
    uint64_t tableId1;
    uint64_t tableId2;
    uint64_t tableId3;
    BindTransport::BindSession* session1;
    BindTransport::BindSession* session2;
    BindTransport::BindSession* session3;
    Tub<ClientTransactionTask::DecisionRpc> decisionRpc;
    Tub<ClientTransactionTask::PrepareRpc> prepareRpc;
    Tub<ClientTransactionTask> transactionTask;

    ClientTransactionTaskTest()
        : logEnabler()
        , context()
        , cluster(&context)
        , ramcloud()
        , tableId1(-1)
        , tableId2(-2)
        , tableId3(-3)
        , session1(NULL)
        , session2(NULL)
        , session3(NULL)
        , decisionRpc()
        , prepareRpc()
        , transactionTask()
    {
        Logger::get().setLogLevels(RAMCloud::SILENT_LOG_LEVEL);

        ServerConfig config = ServerConfig::forTesting();
        config.services = {WireFormat::MASTER_SERVICE,
                           WireFormat::PING_SERVICE};
        config.localLocator = "mock:host=master1";
        config.maxObjectKeySize = 512;
        config.maxObjectDataSize = 1024;
        config.segmentSize = 128*1024;
        config.segletSize = 128*1024;
        cluster.addServer(config);
        config.services = {WireFormat::MASTER_SERVICE,
                           WireFormat::PING_SERVICE};
        config.localLocator = "mock:host=master2";
        cluster.addServer(config);
        config.services = {WireFormat::MASTER_SERVICE,
                           WireFormat::PING_SERVICE};
        config.localLocator = "mock:host=master3";
        cluster.addServer(config);
        ramcloud.construct(&context, "mock:host=coordinator");

        // Get pointers to the master sessions.
        Transport::SessionRef session =
                ramcloud->clientContext->transportManager->getSession(
                "mock:host=master1");
        session1 = static_cast<BindTransport::BindSession*>(session.get());
        session = ramcloud->clientContext->transportManager->getSession(
                "mock:host=master2");
        session2 = static_cast<BindTransport::BindSession*>(session.get());
        session = ramcloud->clientContext->transportManager->getSession(
                "mock:host=master3");
        session3 = static_cast<BindTransport::BindSession*>(session.get());

        transactionTask.construct(ramcloud.get());
        transactionTask->lease = ramcloud->clientLease.getLease();

        prepareRpc.construct(ramcloud.get(), session, transactionTask.get());
        decisionRpc.construct(ramcloud.get(), session, transactionTask.get());

        // Make some tables.
        tableId1 = ramcloud->createTable("table1");
        tableId2 = ramcloud->createTable("table2");
        tableId3 = ramcloud->createTable("table3");
    }

    void populateCommitCache() {
        {
            Key key(8, "01", 2);
            transactionTask->insertCacheEntry(key, "hello 81", 8);
        }
        {
            Key key(1, "01", 2);
            transactionTask->insertCacheEntry(key, "hello 11", 8);
        }
        {
            Key key(2, "01", 2);
            transactionTask->insertCacheEntry(key, "hello 21", 8);
        }
        {
            Key key(2, "01", 2);
            transactionTask->insertCacheEntry(key, "goodbye 21", 10);
        }
        {
            Key key(2, "02", 2);
            transactionTask->insertCacheEntry(key, "hello 22", 8);
        }
        {
            Key key(4, "01", 2);
            transactionTask->insertCacheEntry(key, "hello 41", 8);
        }
    }

    void insertEntry(ClientTransactionTask::CacheEntry::Type type,
            uint64_t tableId, const void* key, uint16_t keyLength,
            const void* buf, uint32_t length)
    {
        Key keyObj(tableId, key, keyLength);
        ClientTransactionTask::CacheEntry* entry =
                transactionTask->findCacheEntry(keyObj);

        if (entry == NULL) {
            entry = transactionTask->insertCacheEntry(keyObj, buf, length);
        } else {
            entry->objectBuf->reset();
            Object::appendKeysAndValueToBuffer(
                    keyObj, buf, length, entry->objectBuf, true);
        }

        entry->type = type;
    }

    void insertRead(uint64_t tableId, const void* key, uint16_t keyLength)
    {
        insertEntry(ClientTransactionTask::CacheEntry::READ,
                tableId, key, keyLength, NULL, 0);
    }

    void insertRemove(uint64_t tableId, const void* key, uint16_t keyLength)
    {
        insertEntry(ClientTransactionTask::CacheEntry::REMOVE,
                tableId, key, keyLength, NULL, 0);
    }

    void insertWrite(uint64_t tableId, const void* key, uint16_t keyLength,
            const void* buf, uint32_t length)
    {
        insertEntry(ClientTransactionTask::CacheEntry::WRITE,
                tableId, key, keyLength, buf, length);
    }

    string participantListToString(ClientTransactionTask* t) {
        string s;
        uint32_t offset = 0;
        s.append(format("ParticipantList["));
        for (uint32_t i = 0; i < t->participantCount; i++) {
            WireFormat::TxParticipant* entry =
                    t->participantList.getOffset<WireFormat::TxParticipant>(
                            offset);
            s.append(format(" {%lu, %lu, %lu}",
                            entry->tableId, entry->keyHash, entry->rpcId));
            offset += sizeof32(WireFormat::TxParticipant);
        }
        s.append(format(" ]"));
        return s;
    }

    string rpcToString(ClientTransactionTask::DecisionRpc* rpc) {
        string s;
        s.append(
                format("DecisionRpc :: lease{%lu}", rpc->reqHdr->leaseId));
        s.append(
                format(" participantCount{%u}", rpc->reqHdr->participantCount));
        uint32_t offset = sizeof(WireFormat::TxDecision::Request);

        s.append(format(" ParticipantList["));
        for (uint32_t i = 0; i < rpc->reqHdr->participantCount; i++) {
            WireFormat::TxParticipant* entry =
                    rpc->request.getOffset<WireFormat::TxParticipant>(offset);
            s.append(format(" {%lu, %lu, %lu}",
                            entry->tableId, entry->keyHash, entry->rpcId));
            offset += sizeof32(WireFormat::TxParticipant);
        }
        s.append(format(" ]"));
        return s;
    }

    string rpcToString(ClientTransactionTask::PrepareRpc* rpc) {
        string s;
        s.append(
                format("PrepareRpc :: lease{%lu}", rpc->reqHdr->lease.leaseId));
        s.append(format(" ackId{%lu} ", rpc->reqHdr->ackId));
        s.append(
                format("participantCount{%u}", rpc->reqHdr->participantCount));
        s.append(format(" opCount{%u}", rpc->reqHdr->opCount));
        uint32_t offset = sizeof(WireFormat::TxPrepare::Request);

        s.append(format(" ParticipantList["));
        for (uint32_t i = 0; i < rpc->reqHdr->participantCount; i++) {
            WireFormat::TxParticipant* entry =
                    rpc->request.getOffset<WireFormat::TxParticipant>(offset);
            s.append(format(" {%lu, %lu, %lu}",
                            entry->tableId, entry->keyHash, entry->rpcId));
            offset += sizeof32(WireFormat::TxParticipant);
        }
        s.append(format(" ]"));
        s.append(format(" OpSet["));
        for (uint32_t i = 0; i < rpc->reqHdr->opCount; i++) {
            WireFormat::TxPrepare::OpType* type =
                    rpc->request.getOffset<
                            WireFormat::TxPrepare::OpType>(offset);
            switch (*type) {
                case WireFormat::TxPrepare::READ:
                {
                    WireFormat::TxPrepare::Request::ReadOp* entry =
                            rpc->request.getOffset<
                                    WireFormat::TxPrepare::Request::ReadOp>(
                                            offset);
                    s.append(format(" READ{%lu, %lu}",
                            entry->tableId, entry->rpcId));
                    offset += sizeof32(WireFormat::TxPrepare::Request::ReadOp);
                    offset += entry->keyLength;
                    break;
                }
                case WireFormat::TxPrepare::REMOVE:
                {
                    WireFormat::TxPrepare::Request::RemoveOp* entry =
                            rpc->request.getOffset<
                                    WireFormat::TxPrepare::Request::RemoveOp>(
                                            offset);
                    s.append(format(" REMOVE{%lu, %lu}",
                            entry->tableId, entry->rpcId));
                    offset +=
                            sizeof32(WireFormat::TxPrepare::Request::RemoveOp);
                    offset += entry->keyLength;
                    break;
                }
                case WireFormat::TxPrepare::WRITE:
                {
                    WireFormat::TxPrepare::Request::WriteOp* entry =
                            rpc->request.getOffset<
                                    WireFormat::TxPrepare::Request::WriteOp>(
                                            offset);
                    s.append(format(" WRITE{%lu, %lu}",
                            entry->tableId, entry->rpcId));
                    offset += sizeof32(WireFormat::TxPrepare::Request::WriteOp);
                    offset += entry->length;
                    break;
                }
                default:
                    break;
            }
        }
        s.append(format(" ]"));
        return s;
    }

    DISALLOW_COPY_AND_ASSIGN(ClientTransactionTaskTest);
};

TEST_F(ClientTransactionTaskTest, commitCache) {
    populateCommitCache();
    uint32_t dataLength = 0;
    const char* str;

    EXPECT_EQ(6U, transactionTask->commitCache.size());
    EXPECT_EQ(1U,
            transactionTask->commitCache.count({1, Key::getHash(1, "01", 2)}));
    EXPECT_EQ(2U,
            transactionTask->commitCache.count({2, Key::getHash(2, "01", 2)}));
    EXPECT_EQ(1U,
            transactionTask->commitCache.count({2, Key::getHash(2, "02", 2)}));
    EXPECT_EQ(1U,
            transactionTask->commitCache.count({4, Key::getHash(4, "01", 2)}));

    ClientTransactionTask::CommitCacheMap::iterator it =
            transactionTask->commitCache.begin();
    EXPECT_EQ(1U, it->first.tableId);
    EXPECT_EQ(Key::getHash(1, "01", 2), it->first.keyHash);
    it++;

    EXPECT_EQ(2U, it->first.tableId);
    EXPECT_EQ(Key::getHash(2, "02", 2), it->first.keyHash);
    it++;

    EXPECT_EQ(2U, it->first.tableId);
    EXPECT_EQ(Key::getHash(2, "01", 2), it->first.keyHash);
    str = reinterpret_cast<const char*>(
            it->second.objectBuf->getValue(&dataLength));
    EXPECT_EQ("hello 21", string(str, dataLength));
    it++;

    EXPECT_EQ(2U, it->first.tableId);
    EXPECT_EQ(Key::getHash(2, "01", 2), it->first.keyHash);
    str = reinterpret_cast<const char*>(
            it->second.objectBuf->getValue(&dataLength));
    EXPECT_EQ("goodbye 21", string(str, dataLength));
    it++;

    EXPECT_EQ(4U, it->first.tableId);
    EXPECT_EQ(Key::getHash(4, "01", 2), it->first.keyHash);
    it++;

    EXPECT_EQ(8U, it->first.tableId);
    EXPECT_EQ(Key::getHash(8, "01", 2), it->first.keyHash);
    it++;

    EXPECT_TRUE(it == transactionTask->commitCache.end());
}

TEST_F(ClientTransactionTaskTest, findCacheEntry_basic) {
    populateCommitCache();
    Key key(1, "01", 2);
    ClientTransactionTask::CacheEntry* entry =
            transactionTask->findCacheEntry(key);

    uint32_t dataLength = 0;
    const char* str;
    Key testKey(
            1, entry->objectBuf->getKey(), entry->objectBuf->getKeyLength());
    EXPECT_EQ(key, testKey);
    str = reinterpret_cast<const char*>(
            entry->objectBuf->getValue(&dataLength));
    EXPECT_EQ("hello 11", string(str, dataLength));
}

TEST_F(ClientTransactionTaskTest, findCacheEntry_notFound) {
    Key key1(1, "test", 4);
    EXPECT_TRUE(transactionTask->findCacheEntry(key1) == NULL);
    Key key3(3, "test", 4);
    EXPECT_TRUE(transactionTask->findCacheEntry(key3) == NULL);
    Key key8(8, "test", 4);
    EXPECT_TRUE(transactionTask->findCacheEntry(key8) == NULL);
}

TEST_F(ClientTransactionTaskTest, findCacheEntry_keyCollision) {
    populateCommitCache();

    uint32_t dataLength = 0;
    const char* str;

    Key key(2, "01", 2);
    ClientTransactionTask::CacheEntry* entry =
            transactionTask->findCacheEntry(key);

    EXPECT_TRUE(entry != NULL);

    Key testKey1(
            2, entry->objectBuf->getKey(), entry->objectBuf->getKeyLength());
    EXPECT_EQ(key, testKey1);
    str = reinterpret_cast<const char*>(
            entry->objectBuf->getValue(&dataLength));
    EXPECT_EQ("hello 21", string(str, dataLength));

    // Sanity check
    ClientTransactionTask::CommitCacheMap::iterator it =
            transactionTask->commitCache.lower_bound({key.getTableId(),
                                                 key.getHash()});

    entry = &it->second;
    str = reinterpret_cast<const char*>(
            entry->objectBuf->getValue(&dataLength));
    EXPECT_EQ("hello 21", string(str, dataLength));

    it++;
    entry = &it->second;
    str = reinterpret_cast<const char*>(
            entry->objectBuf->getValue(&dataLength));
    EXPECT_EQ("goodbye 21", string(str, dataLength));

    // Change first entry so as to not match.
    entry = transactionTask->findCacheEntry(key);
    entry->objectBuf->reset();
    Key badKey(2, "BAD", 3);
    Object::appendKeysAndValueToBuffer(
            badKey, "BAD   11", 8, entry->objectBuf, true);

    entry = transactionTask->findCacheEntry(key);

    EXPECT_TRUE(entry != NULL);

    Key testKey2(
            2, entry->objectBuf->getKey(), entry->objectBuf->getKeyLength());
    EXPECT_EQ(key, testKey2);
    str = reinterpret_cast<const char*>(
            entry->objectBuf->getValue(&dataLength));
    EXPECT_EQ("goodbye 21", string(str, dataLength));

    // Sanity check
    it = transactionTask->commitCache.lower_bound({key.getTableId(),
                                               key.getHash()});

    entry = &it->second;
    str = reinterpret_cast<const char*>(
            entry->objectBuf->getValue(&dataLength));
    EXPECT_EQ("BAD   11", string(str, dataLength));

    it++;
    entry = &it->second;
    str = reinterpret_cast<const char*>(
            entry->objectBuf->getValue(&dataLength));
    EXPECT_EQ("goodbye 21", string(str, dataLength));
}

TEST_F(ClientTransactionTaskTest, findCacheEntry_empty) {
    Key key(1, "test", 4);
    EXPECT_TRUE(transactionTask->findCacheEntry(key) == NULL);
}

TEST_F(ClientTransactionTaskTest, insertCacheEntry) {
    Key key(1, "test", 4);
    ClientTransactionTask::CacheEntry* entry =
            transactionTask->insertCacheEntry(key, "hello world", 11);

    uint32_t dataLength = 0;
    const char* str;
    Key testKey(
            1, entry->objectBuf->getKey(), entry->objectBuf->getKeyLength());
    EXPECT_EQ(key, testKey);
    str = reinterpret_cast<const char*>(
            entry->objectBuf->getValue(&dataLength));
    EXPECT_EQ("hello world", string(str, dataLength));
}

TEST_F(ClientTransactionTaskTest, performTask_basic) {
    // Make sure objects exist first.
    ramcloud->write(tableId1, "test1", 5, "hello", 5);
    ramcloud->write(tableId1, "test2", 5, "hello", 5);
    ramcloud->write(tableId1, "test3", 5, "hello", 5);
    ramcloud->write(tableId1, "test4", 5, "hello", 5);
    ramcloud->write(tableId1, "test5", 5, "hello", 5);
    ramcloud->write(tableId2, "test1", 5, "hello", 5);
    ramcloud->write(tableId3, "test1", 5, "hello", 5);

    insertWrite(tableId1, "test1", 5, "hello", 5);
    insertWrite(tableId1, "test2", 5, "hello", 5);
    insertWrite(tableId1, "test3", 5, "hello", 5);

    insertWrite(tableId1, "test4", 5, "hello", 5);
    insertWrite(tableId1, "test5", 5, "hello", 5);

    insertWrite(tableId2, "test1", 5, "hello", 5);

    insertWrite(tableId3, "test1", 5, "hello", 5);

    EXPECT_EQ(ClientTransactionTask::INIT, transactionTask->state);
    EXPECT_EQ(1, transactionTask->performTask());// RPC 1 Sent, RPC 1 Processed
    EXPECT_EQ(ClientTransactionTask::PREPARE, transactionTask->state);
    transactionTask->performTask();              // RPC 2 Sent, RPC 2 Processed
    EXPECT_EQ(ClientTransactionTask::PREPARE, transactionTask->state);
    transactionTask->performTask();              // RPC 3 Sent, RPC 3 Processed
    EXPECT_EQ(ClientTransactionTask::PREPARE, transactionTask->state);
    transactionTask->performTask();              // RPC 4 Sent, RPC 4 Processed
                                                 // RPC 1 Sent, RPC 1 Processed
    EXPECT_EQ(ClientTransactionTask::DECISION, transactionTask->state);
    EXPECT_EQ(1, transactionTask->performTask()); // RPC 2 Sent, RPC 2 Processed
    EXPECT_EQ(ClientTransactionTask::DECISION, transactionTask->state);
    transactionTask->performTask();              // RPC 3 Sent, RPC 3 Processed
    EXPECT_EQ(ClientTransactionTask::DECISION, transactionTask->state);
    transactionTask->performTask();              // RPC 4 Sent, RPC 4 Processed
    EXPECT_EQ(ClientTransactionTask::DONE, transactionTask->state);
    EXPECT_EQ(0, transactionTask->performTask());
    EXPECT_EQ(ClientTransactionTask::DONE, transactionTask->state);
}

TEST_F(ClientTransactionTaskTest, performTask_setDecision) {
    transactionTask->state = ClientTransactionTask::INIT;
    transactionTask->decision = WireFormat::TxDecision::UNDECIDED;
    transactionTask->performTask();
    EXPECT_EQ(WireFormat::TxDecision::COMMIT, transactionTask->decision);

    transactionTask->state = ClientTransactionTask::INIT;
    transactionTask->decision = WireFormat::TxDecision::COMMIT;
    transactionTask->performTask();
    EXPECT_EQ(WireFormat::TxDecision::COMMIT, transactionTask->decision);

    transactionTask->state = ClientTransactionTask::INIT;
    transactionTask->decision = WireFormat::TxDecision::ABORT;
    transactionTask->performTask();
    EXPECT_EQ(WireFormat::TxDecision::ABORT, transactionTask->decision);
}

TEST_F(ClientTransactionTaskTest, performTask_ClientException) {
    insertWrite(0, "test1", 5, "hello", 5);
    TestLog::reset();

    EXPECT_EQ(ClientTransactionTask::INIT, transactionTask->state);
    EXPECT_EQ(1, transactionTask->performTask());
    EXPECT_EQ(ClientTransactionTask::DONE, transactionTask->state);
    EXPECT_EQ("performTask: Unexpected exception 'table doesn't exist' while "
              "preparing transaction commit; will result in internal error.",
              TestLog::get());
}

TEST_F(ClientTransactionTaskTest, start) {
    std::shared_ptr<ClientTransactionTask>
            taskPtr(new ClientTransactionTask(ramcloud.get()));
    ClientTransactionTask* task = taskPtr.get();
    EXPECT_FALSE(task->poller);
    ClientTransactionTask::start(taskPtr);
    EXPECT_TRUE(task->poller);
    task->poller.destroy();
}

TEST_F(ClientTransactionTaskTest, Poller_poll) {
    std::shared_ptr<ClientTransactionTask>
            taskPtr(new ClientTransactionTask(ramcloud.get()));
    ClientTransactionTask* task = taskPtr.get();
    // Give it something to do.
    ClientTransactionTask::CacheEntry* entry;
    Key key1(tableId1, "test1", 5);
    entry = task->insertCacheEntry(key1, "hello", 5);
    entry->type = ClientTransactionTask::CacheEntry::WRITE;
    Key key2(tableId2, "test2", 5);
    entry = task->insertCacheEntry(key2, "hello", 5);
    entry->type = ClientTransactionTask::CacheEntry::WRITE;

    EXPECT_FALSE(task->isReady());
    EXPECT_EQ(ClientTransactionTask::INIT, task->state);
    EXPECT_FALSE(task->poller);
    ClientTransactionTask::start(taskPtr);
    EXPECT_TRUE(task->poller);
    task->poller->running = true;

    // Nothing should happen.
    EXPECT_EQ(0, task->poller->poll());

    EXPECT_FALSE(task->isReady());
    EXPECT_EQ(ClientTransactionTask::INIT, task->state);
    EXPECT_TRUE(task->poller);
    task->poller->running = false;

    // Should move to PREPARE
    EXPECT_EQ(1, task->poller->poll());

    EXPECT_FALSE(task->isReady());
    EXPECT_EQ(ClientTransactionTask::PREPARE, task->state);
    EXPECT_TRUE(task->poller);

    // Should move to DECISION
    EXPECT_EQ(1, task->poller->poll());

    EXPECT_FALSE(task->isReady());
    EXPECT_EQ(ClientTransactionTask::DECISION, task->state);
    EXPECT_TRUE(task->poller);

    // Should move to DONE
    EXPECT_EQ(1, task->poller->poll());

    EXPECT_TRUE(task->isReady());
    EXPECT_EQ(ClientTransactionTask::DONE, task->state);
    EXPECT_FALSE(task->poller);
}

TEST_F(ClientTransactionTaskTest, initTask) {
    insertWrite(1, "test", 4, "hello", 5);
    insertWrite(2, "test", 4, "hello", 5);
    insertWrite(3, "test", 4, "hello", 5);

    transactionTask->initTask();
    EXPECT_EQ(1U, transactionTask->txId);
    EXPECT_EQ("ParticipantList[ {1, 14087593745509316690, 1}"
                              " {2, 2793085152624492990, 2}"
                              " {3, 17667676865770333572, 3} ]",
              participantListToString(transactionTask.get()));
}

TEST_F(ClientTransactionTaskTest, processDecisionRpcResults_basic) {
    // Must make sure the object exists before we try to transaction on it.
    ramcloud->write(tableId1, "test", 4, "hello", 5);

    insertWrite(tableId1, "test", 4, "hello", 5);
    transactionTask->initTask();
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    transactionTask->decision = WireFormat::TxDecision::COMMIT;
    transactionTask->sendDecisionRpc();

    EXPECT_EQ(1U, transactionTask->decisionRpcs.size());
    TestLog::reset();
    EXPECT_EQ(1, transactionTask->processDecisionRpcResults());
    EXPECT_EQ("processDecisionRpcResults: STATUS_OK", TestLog::get());
    EXPECT_EQ(0U, transactionTask->decisionRpcs.size());
}

TEST_F(ClientTransactionTaskTest, processDecisionRpcResults_unknownTablet) {
    // Must make sure the object exists before we try to transaction on it.
    ramcloud->write(tableId1, "test", 4, "hello", 5);

    insertWrite(tableId1, "test", 4, "hello", 5);
    transactionTask->initTask();
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    transactionTask->decision = WireFormat::TxDecision::COMMIT;
    transactionTask->sendDecisionRpc();
    // Resend to wrong session
    transactionTask->decisionRpcs.begin()->session = session2;
    transactionTask->decisionRpcs.begin()->send();

    EXPECT_EQ(1U, transactionTask->decisionRpcs.size());
    TestLog::reset();
    EXPECT_EQ(1, transactionTask->processDecisionRpcResults());
    EXPECT_EQ("processDecisionRpcResults: STATUS_UNKNOWN_TABLET",
              TestLog::get());
    EXPECT_EQ(0U, transactionTask->decisionRpcs.size());
}

TEST_F(ClientTransactionTaskTest, processDecisionRpcResults_failed) {
    // Must make sure the object exists before we try to transaction on it.
    ramcloud->write(tableId1, "test", 4, "hello", 5);

    insertWrite(tableId1, "test", 4, "hello", 5);
    transactionTask->initTask();
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    transactionTask->sendDecisionRpc();
    // Fail the rpc.
    transactionTask->decisionRpcs.begin()->failed();

    EXPECT_EQ(1U, transactionTask->decisionRpcs.size());
    TestLog::reset();
    EXPECT_EQ(1, transactionTask->processDecisionRpcResults());
    EXPECT_EQ("flushSession: flushing session for mock:host=master1 | "
              "processDecisionRpcResults: FAILED", TestLog::get());
    EXPECT_EQ(0U, transactionTask->decisionRpcs.size());
}

TEST_F(ClientTransactionTaskTest, processDecisionRpcResults_notReady) {
    // Must make sure the object exists before we try to transaction on it.
    ramcloud->write(tableId1, "test", 4, "hello", 5);

    insertWrite(tableId1, "test", 4, "hello", 5);
    transactionTask->initTask();
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    transactionTask->sendDecisionRpc();

    EXPECT_EQ(1U, transactionTask->decisionRpcs.size());
    transactionTask->decisionRpcs.begin()->state =
            ClientTransactionTask::DecisionRpc::IN_PROGRESS;
    EXPECT_EQ(0, transactionTask->processDecisionRpcResults());
    EXPECT_EQ(1U, transactionTask->decisionRpcs.size());
}

// TODO(cstlee) : Unit test processDecisionRpcResults_badStatus

TEST_F(ClientTransactionTaskTest, processPrepareRpcResults_basic) {
    // Must make sure the object exists before we try to transaction on it.
    ramcloud->write(tableId1, "test", 4, "hello", 5);

    insertWrite(tableId1, "test", 4, "hello", 5);
    transactionTask->initTask();
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    transactionTask->sendPrepareRpc();

    EXPECT_EQ(1U, transactionTask->prepareRpcs.size());
    EXPECT_EQ(WireFormat::TxDecision::UNDECIDED, transactionTask->decision);
    EXPECT_EQ(1, transactionTask->processPrepareRpcResults());
    EXPECT_EQ(0U, transactionTask->prepareRpcs.size());
    EXPECT_EQ(WireFormat::TxDecision::UNDECIDED, transactionTask->decision);
}

TEST_F(ClientTransactionTaskTest, processPrepareRpcResults_abort) {
    // Must make sure the object exists before we try to transaction on it.
    ramcloud->write(tableId1, "test", 4, "hello", 5);

    // Start another transaction to cause a lock
    {
        ClientTransactionTask task(ramcloud.get());
        Key key(tableId1, "test", 4);
        ClientTransactionTask::CacheEntry* entry =
                task.insertCacheEntry(key, "hello", 5);
        entry->type = ClientTransactionTask::CacheEntry::WRITE;
        task.initTask();
        task.nextCacheEntry = task.commitCache.begin();
        task.sendPrepareRpc();
    }

    insertWrite(tableId1, "test", 4, "hello", 5);
    transactionTask->initTask();
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    transactionTask->sendPrepareRpc();

    EXPECT_EQ(1U, transactionTask->prepareRpcs.size());
    EXPECT_EQ(WireFormat::TxDecision::UNDECIDED, transactionTask->decision);
    EXPECT_EQ(1, transactionTask->processPrepareRpcResults());
    EXPECT_EQ(0U, transactionTask->prepareRpcs.size());
    EXPECT_EQ(WireFormat::TxDecision::ABORT, transactionTask->decision);
}

TEST_F(ClientTransactionTaskTest, processPrepareRpcResults_unknownTablet) {
    // Must make sure the object exists before we try to transaction on it.
    ramcloud->write(tableId1, "test", 4, "hello", 5);

    insertWrite(tableId1, "test", 4, "hello", 5);
    transactionTask->initTask();
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    transactionTask->sendPrepareRpc();
    // Resend to wrong session
    transactionTask->prepareRpcs.begin()->session = session2;
    transactionTask->prepareRpcs.begin()->send();

    EXPECT_EQ(1U, transactionTask->prepareRpcs.size());
    EXPECT_EQ(WireFormat::TxDecision::UNDECIDED, transactionTask->decision);
    TestLog::reset();
    EXPECT_EQ(1, transactionTask->processPrepareRpcResults());
    EXPECT_EQ("processPrepareRpcResults: STATUS_UNKNOWN_TABLET",
              TestLog::get());
    EXPECT_EQ(0U, transactionTask->prepareRpcs.size());
}

TEST_F(ClientTransactionTaskTest, processPrepareRpcResults_failed) {
    ramcloud->write(tableId1, "test", 4, "hello", 5);

    insertWrite(tableId1, "test", 4, "hello", 5);
    transactionTask->initTask();
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    transactionTask->sendPrepareRpc();
    // Fail the rpc.
    transactionTask->prepareRpcs.begin()->failed();

    EXPECT_EQ(1U, transactionTask->prepareRpcs.size());
    EXPECT_EQ(WireFormat::TxDecision::UNDECIDED, transactionTask->decision);
    TestLog::reset();
    EXPECT_EQ(1, transactionTask->processPrepareRpcResults());
    EXPECT_EQ("flushSession: flushing session for mock:host=master1 | "
              "processPrepareRpcResults: FAILED", TestLog::get());
    EXPECT_EQ(0U, transactionTask->prepareRpcs.size());
}

// TODO(cstlee) : Unit test processPrepareRpcResults_badStatus
//TEST_F(ClientTransactionTaskTest, processPrepareRpcResults_badStatus) {
//    // Must make sure the object exists before we try to transaction on it.
//    ramcloud->write(tableId1, "test", 4, "hello", 5);
//
//    insertWrite(tableId1, "test", 4, "hello", 5);
//    transactionTask->initTask();
//    transactionTask->lease.leaseId = 0;
//    transactionTask->lease.leaseExpiration = 0;
//    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
//    transactionTask->sendPrepareRpc();
//
//    EXPECT_EQ(1U, transactionTask->prepareRpcs.size());
//    EXPECT_EQ(WireFormat::TxDecision::COMMIT, transactionTask->decision);
//    EXPECT_EQ(STATUS_OK, transactionTask->status);
//    transactionTask->processPrepareRpcResults();
//    EXPECT_EQ(0U, transactionTask->prepareRpcs.size());
//    EXPECT_EQ(WireFormat::TxDecision::ABORT, transactionTask->decision);
//    EXPECT_EQ(STATUS_OBJECT_DOESNT_EXIST, transactionTask->status);
//}

TEST_F(ClientTransactionTaskTest, processPrepareRpcResults_notReady) {
    // Must make sure the object exists before we try to transaction on it.
    ramcloud->write(tableId1, "test", 4, "hello", 5);

    insertWrite(tableId1, "test", 4, "hello", 5);
    transactionTask->initTask();
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    transactionTask->sendPrepareRpc();

    EXPECT_EQ(1U, transactionTask->prepareRpcs.size());
    EXPECT_EQ(WireFormat::TxDecision::UNDECIDED, transactionTask->decision);
    transactionTask->prepareRpcs.begin()->state =
            ClientTransactionTask::PrepareRpc::IN_PROGRESS;

    EXPECT_EQ(0, transactionTask->processPrepareRpcResults());
    EXPECT_EQ(1U, transactionTask->prepareRpcs.size());
    EXPECT_EQ(WireFormat::TxDecision::UNDECIDED, transactionTask->decision);
}

TEST_F(ClientTransactionTaskTest, sendDecisionRpc_basic) {
    insertWrite(tableId1, "test1", 5, "hello", 5);
    insertWrite(tableId1, "test2", 5, "hello", 5);
    insertWrite(tableId1, "test3", 5, "hello", 5);
    insertWrite(tableId1, "test4", 5, "hello", 5);
    insertWrite(tableId1, "test5", 5, "hello", 5);
    insertWrite(tableId2, "test1", 5, "hello", 5);
    insertWrite(tableId3, "test1", 5, "hello", 5);

    ClientTransactionTask::DecisionRpc* rpc;
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();

    EXPECT_EQ(0U, transactionTask->decisionRpcs.size());

    // Should issue 1 rpc to master 1 with 3 objects in it.
    EXPECT_EQ(1, transactionTask->sendDecisionRpc());
    EXPECT_EQ(1U, transactionTask->decisionRpcs.size());
    rpc = &transactionTask->decisionRpcs.back();
    EXPECT_EQ(3U, rpc->reqHdr->participantCount);
    EXPECT_EQ("mock:host=master1", rpc->session.get()->serviceLocator);
    EXPECT_EQ("DecisionRpc :: lease{1} participantCount{3} "
              "ParticipantList[ {1, 2318870434438256899, 0} "
                               "{1, 5620473113711160829, 0} "
                               "{1, 8393261455223623089, 0} ]",
              rpcToString(rpc));

    // Rest nextCacheEntry to make see if processed ops will be skipped.
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();

    // Should issue 1 rpc to master 1 with 2 objects in it.
    EXPECT_EQ(1, transactionTask->sendDecisionRpc());
    EXPECT_EQ(2U, transactionTask->decisionRpcs.size());
    rpc = &transactionTask->decisionRpcs.back();
    EXPECT_EQ(2U, rpc->reqHdr->participantCount);
    EXPECT_EQ("mock:host=master1", rpc->session.get()->serviceLocator);
    EXPECT_EQ("DecisionRpc :: lease{1} participantCount{2} "
              "ParticipantList[ {1, 9099387403658286820, 0} "
                               "{1, 17025739677450802839, 0} ]",
              rpcToString(rpc));

    // Should issue 1 rpc to master 2 with 1 objects in it.
    EXPECT_EQ(1, transactionTask->sendDecisionRpc());
    EXPECT_EQ(3U, transactionTask->decisionRpcs.size());
    rpc = &transactionTask->decisionRpcs.back();
    EXPECT_EQ(1U, rpc->reqHdr->participantCount);
    EXPECT_EQ("mock:host=master2", rpc->session.get()->serviceLocator);
    EXPECT_EQ("DecisionRpc :: lease{1} participantCount{1} "
              "ParticipantList[ {2, 8137432257469122462, 0} ]",
              rpcToString(rpc));

    // Should issue 1 rpc to master 3 with 1 objects in it.
    EXPECT_EQ(1, transactionTask->sendDecisionRpc());
    EXPECT_EQ(4U, transactionTask->decisionRpcs.size());
    rpc = &transactionTask->decisionRpcs.back();
    EXPECT_EQ(1U, rpc->reqHdr->participantCount);
    EXPECT_EQ("mock:host=master3", rpc->session.get()->serviceLocator);
    EXPECT_EQ("DecisionRpc :: lease{1} participantCount{1} "
              "ParticipantList[ {3, 17123020360203364791, 0} ]",
              rpcToString(rpc));

    // Should issue nothing.
    EXPECT_EQ(0, transactionTask->sendDecisionRpc());
    EXPECT_EQ(4U, transactionTask->decisionRpcs.size());
}

TEST_F(ClientTransactionTaskTest, sendDecisionRpc_TableDoesntExist) {
    insertWrite(0, "test1", 5, "hello", 5);
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    EXPECT_THROW(transactionTask->sendDecisionRpc(),
                 TableDoesntExistException);
}

TEST_F(ClientTransactionTaskTest, sendPrepareRpc_basic) {
    insertWrite(tableId1, "test1", 5, "hello", 5);
    insertWrite(tableId1, "test2", 5, "hello", 5);
    insertWrite(tableId1, "test3", 5, "hello", 5);
    insertWrite(tableId1, "test4", 5, "hello", 5);
    insertWrite(tableId1, "test5", 5, "hello", 5);
    insertWrite(tableId2, "test1", 5, "hello", 5);
    insertWrite(tableId3, "test1", 5, "hello", 5);

    ClientTransactionTask::PrepareRpc* rpc;
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();

    EXPECT_EQ(0U, transactionTask->prepareRpcs.size());

    // Should issue 1 rpc to master 1 with 3 objects in it.
    EXPECT_EQ(1, transactionTask->sendPrepareRpc());
    EXPECT_EQ(1U, transactionTask->prepareRpcs.size());
    rpc = &transactionTask->prepareRpcs.back();
    EXPECT_EQ(3U, rpc->reqHdr->opCount);
    EXPECT_EQ("mock:host=master1", rpc->session.get()->serviceLocator);
    EXPECT_EQ("PrepareRpc :: lease{1} ackId{0} participantCount{0} opCount{3} "
              "ParticipantList[ ] OpSet[ WRITE{1, 0} WRITE{1, 0} WRITE{1, 0} ]",
              rpcToString(rpc));

    // Rest nextCacheEntry to make see if processed ops will be skipped.
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();

    // Should issue 1 rpc to master 1 with 2 objects in it.
    EXPECT_EQ(1, transactionTask->sendPrepareRpc());
    EXPECT_EQ(2U, transactionTask->prepareRpcs.size());
    rpc = &transactionTask->prepareRpcs.back();
    EXPECT_EQ(2U, rpc->reqHdr->opCount);
    EXPECT_EQ("mock:host=master1", rpc->session.get()->serviceLocator);
    EXPECT_EQ("PrepareRpc :: lease{1} ackId{0} participantCount{0} opCount{2} "
              "ParticipantList[ ] OpSet[ WRITE{1, 0} WRITE{1, 0} ]",
              rpcToString(rpc));

    // Should issue 1 rpc to master 2 with 1 objects in it.
    EXPECT_EQ(1, transactionTask->sendPrepareRpc());
    EXPECT_EQ(3U, transactionTask->prepareRpcs.size());
    rpc = &transactionTask->prepareRpcs.back();
    EXPECT_EQ(1U, rpc->reqHdr->opCount);
    EXPECT_EQ("mock:host=master2", rpc->session.get()->serviceLocator);
    EXPECT_EQ("PrepareRpc :: lease{1} ackId{0} participantCount{0} opCount{1} "
              "ParticipantList[ ] OpSet[ WRITE{2, 0} ]", rpcToString(rpc));

    // Should issue 1 rpc to master 3 with 1 objects in it.
    EXPECT_EQ(1, transactionTask->sendPrepareRpc());
    EXPECT_EQ(4U, transactionTask->prepareRpcs.size());
    rpc = &transactionTask->prepareRpcs.back();
    EXPECT_EQ(1U, rpc->reqHdr->opCount);
    EXPECT_EQ("mock:host=master3", rpc->session.get()->serviceLocator);
    EXPECT_EQ("PrepareRpc :: lease{1} ackId{0} participantCount{0} opCount{1} "
              "ParticipantList[ ] OpSet[ WRITE{3, 0} ]", rpcToString(rpc));

    // Should issue nothing.
    EXPECT_EQ(0, transactionTask->sendPrepareRpc());
    EXPECT_EQ(4U, transactionTask->prepareRpcs.size());
}

TEST_F(ClientTransactionTaskTest, sendPrepareRpc_TableDoesntExist) {
    insertWrite(0, "test1", 5, "hello", 5);
    transactionTask->nextCacheEntry = transactionTask->commitCache.begin();
    EXPECT_THROW(transactionTask->sendPrepareRpc(),
                 TableDoesntExistException);
}

TEST_F(ClientTransactionTaskTest, DecisionRpc_constructor) {
    Transport::SessionRef session =
                ramcloud->clientContext->transportManager->getSession(
                "mock:host=master1");
    transactionTask->decision = WireFormat::TxDecision::ABORT;
    transactionTask->lease.leaseId = 42;
    transactionTask->participantCount = 2;

    ClientTransactionTask::DecisionRpc rpc(
            ramcloud.get(), session, transactionTask.get());
    EXPECT_EQ(transactionTask->decision, rpc.reqHdr->decision);
    EXPECT_EQ(transactionTask->lease.leaseId, rpc.reqHdr->leaseId);
    EXPECT_EQ(0U, rpc.reqHdr->participantCount);
}

TEST_F(ClientTransactionTaskTest, DecisionRpc_checkStatus) {
    insertRemove(1, "test", 4);
    transactionTask->nextCacheEntry = transactionTask->commitCache.end();
    WireFormat::TxPrepare::Response resp;
    resp.common.status = STATUS_TABLE_DOESNT_EXIST;
    decisionRpc->responseHeader = &resp.common;
    decisionRpc->checkStatus();
    EXPECT_EQ(transactionTask->commitCache.end(),
              transactionTask->nextCacheEntry);
    resp.common.status = STATUS_UNKNOWN_TABLET;
    decisionRpc->checkStatus();
    EXPECT_EQ(transactionTask->commitCache.begin(),
              transactionTask->nextCacheEntry);
}

TEST_F(ClientTransactionTaskTest, DecisionRpc_handleTransportError) {
    insertRemove(1, "test", 4);
    transactionTask->nextCacheEntry = transactionTask->commitCache.end();
    TestLog::reset();
    decisionRpc->handleTransportError();
    EXPECT_TRUE(decisionRpc->session == NULL);
    EXPECT_EQ(transactionTask->commitCache.begin(),
              transactionTask->nextCacheEntry);
    EXPECT_EQ("flushSession: flushing session for mock:host=master3",
              TestLog::get());
}

TEST_F(ClientTransactionTaskTest, DecisionRpc_send) {
    EXPECT_TRUE(RpcWrapper::NOT_STARTED == decisionRpc->state);
    decisionRpc->send();
    EXPECT_TRUE(RpcWrapper::NOT_STARTED != decisionRpc->state);
}

TEST_F(ClientTransactionTaskTest, DecisionRpc_appendOp) {
    insertRead(tableId1, "0", 1);

    ClientTransactionTask::CommitCacheMap::iterator it =
            transactionTask->commitCache.begin();
    it->second.rpcId = 42;
    EXPECT_EQ(ClientTransactionTask::CacheEntry::PENDING, it->second.state);
    decisionRpc->appendOp(it);
    EXPECT_EQ(ClientTransactionTask::CacheEntry::DECIDE, it->second.state);
    EXPECT_EQ(decisionRpc->ops[decisionRpc->reqHdr->participantCount - 1], it);
    EXPECT_EQ("DecisionRpc :: lease{1} participantCount{1} "
              "ParticipantList[ {1, 3894390131083956718, 42} ]",
              rpcToString(decisionRpc.get()));
}

TEST_F(ClientTransactionTaskTest, DecisionRpc_retryRequest) {
    insertWrite(1, "test", 4, "hello", 5);
    insertWrite(2, "test", 4, "hello", 5);
    insertWrite(3, "test", 4, "hello", 5);
    ClientTransactionTask::CommitCacheMap::iterator it =
            transactionTask->commitCache.begin();
    while (it != transactionTask->commitCache.end()) {
        decisionRpc->appendOp(it);
        EXPECT_EQ(ClientTransactionTask::CacheEntry::DECIDE, it->second.state);
        it++;
    }

    decisionRpc->retryRequest();

    it = transactionTask->commitCache.begin();
    while (it != transactionTask->commitCache.end()) {
        EXPECT_EQ(ClientTransactionTask::CacheEntry::PENDING, it->second.state);
        it++;
    }
}

TEST_F(ClientTransactionTaskTest, PrepareRpc_constructor) {
    Transport::SessionRef session =
                ramcloud->clientContext->transportManager->getSession(
                "mock:host=master1");
    transactionTask->lease.leaseId = 42;
    transactionTask->participantCount = 2;
    transactionTask->participantList.emplaceAppend<WireFormat::TxParticipant>(
            1, 2, 3);
    transactionTask->participantList.emplaceAppend<WireFormat::TxParticipant>(
            4, 5, 6);

    ClientTransactionTask::PrepareRpc rpc(
            ramcloud.get(), session, transactionTask.get());
    EXPECT_EQ(transactionTask->lease.leaseId, rpc.reqHdr->lease.leaseId);
    EXPECT_EQ(0U, rpc.reqHdr->ackId);
    EXPECT_EQ(transactionTask->participantCount, rpc.reqHdr->participantCount);
    EXPECT_EQ("PrepareRpc :: lease{42} ackId{0} participantCount{2} opCount{0}"
              " ParticipantList[ {1, 2, 3} {4, 5, 6} ] OpSet[ ]",
              rpcToString(&rpc));
}

TEST_F(ClientTransactionTaskTest, PrepareRpc_checkStatus) {
    insertRemove(1, "test", 4);
    transactionTask->nextCacheEntry = transactionTask->commitCache.end();
    WireFormat::TxPrepare::Response resp;
    resp.common.status = STATUS_TABLE_DOESNT_EXIST;
    prepareRpc->responseHeader = &resp.common;
    prepareRpc->checkStatus();
    EXPECT_EQ(transactionTask->commitCache.end(),
              transactionTask->nextCacheEntry);
    resp.common.status = STATUS_UNKNOWN_TABLET;
    prepareRpc->checkStatus();
    EXPECT_EQ(transactionTask->commitCache.begin(),
              transactionTask->nextCacheEntry);
}

TEST_F(ClientTransactionTaskTest, PrepareRpc_handleTransportError) {
    insertRemove(1, "test", 4);
    transactionTask->nextCacheEntry = transactionTask->commitCache.end();
    TestLog::reset();
    prepareRpc->handleTransportError();
    EXPECT_TRUE(prepareRpc->session == NULL);
    EXPECT_EQ(transactionTask->commitCache.begin(),
              transactionTask->nextCacheEntry);
    EXPECT_EQ("flushSession: flushing session for mock:host=master3",
              TestLog::get());
}

TEST_F(ClientTransactionTaskTest, PrepareRpc_send) {
    EXPECT_EQ(0U, prepareRpc->reqHdr->ackId);
    EXPECT_TRUE(RpcWrapper::NOT_STARTED == prepareRpc->state);
    prepareRpc->send();
    EXPECT_EQ(ramcloud->rpcTracker.ackId(), prepareRpc->reqHdr->ackId);
    EXPECT_TRUE(RpcWrapper::NOT_STARTED != prepareRpc->state);
}

TEST_F(ClientTransactionTaskTest, PrepareRpc_appendOp_read) {
    insertRead(tableId1, "0", 1);

    ClientTransactionTask::CommitCacheMap::iterator it =
            transactionTask->commitCache.begin();
    it->second.rpcId = 42;
    EXPECT_EQ(ClientTransactionTask::CacheEntry::PENDING, it->second.state);
    prepareRpc->appendOp(it);
    EXPECT_EQ(ClientTransactionTask::CacheEntry::PREPARE, it->second.state);
    EXPECT_EQ(prepareRpc->ops[prepareRpc->reqHdr->opCount - 1], it);
    EXPECT_EQ("PrepareRpc :: lease{1} ackId{0} participantCount{0} opCount{1} "
              "ParticipantList[ ] OpSet[ READ{1, 42} ]",
              rpcToString(prepareRpc.get()));
}

TEST_F(ClientTransactionTaskTest, PrepareRpc_appendOp_remove) {
    insertRemove(2, "test", 4);

    ClientTransactionTask::CommitCacheMap::iterator it =
            transactionTask->commitCache.begin();
    it->second.rpcId = 42;
    EXPECT_EQ(ClientTransactionTask::CacheEntry::PENDING, it->second.state);
    prepareRpc->appendOp(it);
    EXPECT_EQ(ClientTransactionTask::CacheEntry::PREPARE, it->second.state);
    EXPECT_EQ(prepareRpc->ops[prepareRpc->reqHdr->opCount - 1], it);
    EXPECT_EQ("PrepareRpc :: lease{1} ackId{0} participantCount{0} opCount{1} "
              "ParticipantList[ ] OpSet[ REMOVE{2, 42} ]",
              rpcToString(prepareRpc.get()));
}

TEST_F(ClientTransactionTaskTest, PrepareRpc_appendOp_write) {
    insertWrite(3, "test", 4, "hello", 5);

    ClientTransactionTask::CommitCacheMap::iterator it =
            transactionTask->commitCache.begin();
    it->second.rpcId = 42;
    EXPECT_EQ(ClientTransactionTask::CacheEntry::PENDING, it->second.state);
    prepareRpc->appendOp(it);
    EXPECT_EQ(ClientTransactionTask::CacheEntry::PREPARE, it->second.state);
    EXPECT_EQ(prepareRpc->ops[prepareRpc->reqHdr->opCount - 1], it);
    EXPECT_EQ("PrepareRpc :: lease{1} ackId{0} participantCount{0} opCount{1} "
              "ParticipantList[ ] OpSet[ WRITE{3, 42} ]",
              rpcToString(prepareRpc.get()));
}

TEST_F(ClientTransactionTaskTest, PrepareRpc_appendOp_invalid) {
    insertWrite(3, "test", 4, "hello", 5);

    ClientTransactionTask::CommitCacheMap::iterator it =
            transactionTask->commitCache.begin();
    it->second.rpcId = 42;
    it->second.type = ClientTransactionTask::CacheEntry::INVALID;
    EXPECT_EQ(ClientTransactionTask::CacheEntry::PENDING, it->second.state);
    TestLog::reset();
    prepareRpc->appendOp(it);
    EXPECT_EQ(ClientTransactionTask::CacheEntry::PENDING, it->second.state);
    EXPECT_EQ(0U, prepareRpc->reqHdr->opCount);
    EXPECT_EQ("PrepareRpc :: lease{1} ackId{0} participantCount{0} opCount{0} "
              "ParticipantList[ ] OpSet[ ]", rpcToString(prepareRpc.get()));
    EXPECT_EQ("appendOp: Unknown transaction op type.", TestLog::get());
}

TEST_F(ClientTransactionTaskTest, PrepareRpc_retryRequest) {
    insertWrite(1, "test", 4, "hello", 5);
    insertWrite(2, "test", 4, "hello", 5);
    insertWrite(3, "test", 4, "hello", 5);
    ClientTransactionTask::CommitCacheMap::iterator it =
            transactionTask->commitCache.begin();
    while (it != transactionTask->commitCache.end()) {
        prepareRpc->appendOp(it);
        EXPECT_EQ(ClientTransactionTask::CacheEntry::PREPARE, it->second.state);
        it++;
    }

    prepareRpc->retryRequest();

    it = transactionTask->commitCache.begin();
    while (it != transactionTask->commitCache.end()) {
        EXPECT_EQ(ClientTransactionTask::CacheEntry::PENDING, it->second.state);
        it++;
    }
}

}  // namespace RAMCloud

