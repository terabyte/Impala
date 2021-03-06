// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "simple-scheduler.h"
#include "common/logging.h"
#include "simple-scheduler-test-util.h"
#include "testutil/gtest-util.h"

using namespace impala;
using namespace impala::test;

namespace impala {

class SchedulerTest : public testing::Test {
 protected:
  SchedulerTest() { srand(0); };
};

/// Smoke test to schedule a single table with a single scan range over a single host.
TEST_F(SchedulerTest, SingleHostSingleFile) {
  Cluster cluster;
  cluster.AddHost(true, true);

  Schema schema(cluster);
  schema.AddMultiBlockTable("T", 1, ReplicaPlacement::LOCAL_ONLY, 1);

  Plan plan(schema);
  plan.AddTableScan("T");

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  scheduler.Compute(&result);

  EXPECT_EQ(1, result.NumTotalAssignments());
  EXPECT_EQ(1 * Block::DEFAULT_BLOCK_SIZE, result.NumTotalAssignedBytes());
  EXPECT_EQ(1, result.NumTotalAssignments(0));
  EXPECT_EQ(1 * Block::DEFAULT_BLOCK_SIZE, result.NumTotalAssignedBytes(0));
  EXPECT_EQ(0, result.NumCachedAssignments());
}

/// Test assigning all scan ranges to the coordinator.
TEST_F(SchedulerTest, ExecAtCoord) {
  Cluster cluster;
  cluster.AddHosts(3, true, true);

  Schema schema(cluster);
  schema.AddMultiBlockTable("T", 3, ReplicaPlacement::LOCAL_ONLY, 3);

  Plan plan(schema);
  plan.AddTableScan("T");

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  bool exec_at_coord = true;
  scheduler.Compute(exec_at_coord, &result);

  EXPECT_EQ(3 * Block::DEFAULT_BLOCK_SIZE, result.NumTotalAssignedBytes(0));
  EXPECT_EQ(0, result.NumTotalAssignedBytes(1));
  EXPECT_EQ(0, result.NumTotalAssignedBytes(2));
}

/// Test scanning a simple table twice.
TEST_F(SchedulerTest, ScanTableTwice) {
  Cluster cluster;
  cluster.AddHosts(3, true, true);

  Schema schema(cluster);
  schema.AddMultiBlockTable("T", 2, ReplicaPlacement::LOCAL_ONLY, 3);

  Plan plan(schema);
  plan.AddTableScan("T");
  plan.AddTableScan("T");

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  scheduler.Compute(&result);

  EXPECT_EQ(4 * Block::DEFAULT_BLOCK_SIZE, result.NumTotalAssignedBytes());
  EXPECT_EQ(4 * Block::DEFAULT_BLOCK_SIZE, result.NumDiskAssignedBytes());
  EXPECT_EQ(0, result.NumCachedAssignedBytes());
}

// TODO: This test can be removed once we have the non-random backend round-robin by rank.
/// Schedule randomly over 3 backends and ensure that each backend is at least used once.
TEST_F(SchedulerTest, RandomReads) {
  Cluster cluster;
  cluster.AddHosts(3, true, true);

  Schema schema(cluster);
  schema.AddSingleBlockTable("T1", {0, 1, 2});

  Plan plan(schema);
  plan.AddTableScan("T1");
  plan.SetRandomReplica(true);

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  for (int i = 0; i < 100; ++i) scheduler.Compute(&result);

  ASSERT_EQ(100, result.NumAssignments());
  EXPECT_EQ(100, result.NumTotalAssignments());
  EXPECT_EQ(100 * Block::DEFAULT_BLOCK_SIZE, result.NumTotalAssignedBytes());
  EXPECT_EQ(3, result.NumDistinctBackends());
  EXPECT_GE(result.MinNumAssignedBytesPerHost(), Block::DEFAULT_BLOCK_SIZE);
}

/// Distribute a table over the first 3 nodes in the cluster and verify that repeated
/// schedules always pick the first replica (random_replica = false).
TEST_F(SchedulerTest, LocalReadsPickFirstReplica) {
  Cluster cluster;
  for (int i = 0; i < 10; ++i) cluster.AddHost(i < 5, true);

  Schema schema(cluster);
  schema.AddSingleBlockTable("T1", {0, 1, 2});

  Plan plan(schema);
  plan.AddTableScan("T1");
  plan.SetRandomReplica(false);

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  for (int i = 0; i < 3; ++i) scheduler.Compute(&result);

  EXPECT_EQ(3, result.NumTotalAssignments());
  EXPECT_EQ(3, result.NumDiskAssignments(0));
  EXPECT_EQ(0, result.NumDiskAssignments(1));
  EXPECT_EQ(0, result.NumDiskAssignments(2));
}

/// Create a medium sized cluster with 100 nodes and compute a schedule over 3 tables.
TEST_F(SchedulerTest, TestMediumSizedCluster) {
  Cluster cluster;
  cluster.AddHosts(100, true, true);

  Schema schema(cluster);
  schema.AddMultiBlockTable("T1", 10, ReplicaPlacement::LOCAL_ONLY, 3);
  schema.AddMultiBlockTable("T2", 5, ReplicaPlacement::LOCAL_ONLY, 3);
  schema.AddMultiBlockTable("T3", 1, ReplicaPlacement::LOCAL_ONLY, 3);

  Plan plan(schema);
  plan.AddTableScan("T1");
  plan.AddTableScan("T2");
  plan.AddTableScan("T3");

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  scheduler.Compute(&result);

  EXPECT_EQ(16, result.NumTotalAssignments());
  EXPECT_EQ(16, result.NumDiskAssignments());
}

/// Verify that remote placement and scheduling work as expected.
TEST_F(SchedulerTest, RemoteOnlyPlacement) {
  Cluster cluster;
  for (int i = 0; i < 100; ++i) cluster.AddHost(i < 30, true);

  Schema schema(cluster);
  schema.AddMultiBlockTable("T1", 10, ReplicaPlacement::REMOTE_ONLY, 3);

  Plan plan(schema);
  plan.AddTableScan("T1");

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  scheduler.Compute(&result);

  EXPECT_EQ(10, result.NumTotalAssignments());
  EXPECT_EQ(10, result.NumRemoteAssignments());
  EXPECT_EQ(Block::DEFAULT_BLOCK_SIZE, result.MaxNumAssignedBytesPerHost());
}

/// Add a table with 1000 scan ranges over 10 hosts and ensure that the right number of
/// assignments is computed.
TEST_F(SchedulerTest, ManyScanRanges) {
  Cluster cluster;
  cluster.AddHosts(10, true, true);

  Schema schema(cluster);
  schema.AddMultiBlockTable("T", 1000, ReplicaPlacement::LOCAL_ONLY, 3);

  Plan plan(schema);
  plan.AddTableScan("T");

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  scheduler.Compute(&result);

  EXPECT_EQ(1000, result.NumTotalAssignments());
  EXPECT_EQ(1000, result.NumDiskAssignments());
  // When distributing 1000 blocks with 1 replica over 10 hosts, the probability for the
  // most-picked host to end up with more than 140 blocks is smaller than 1E-3 (Chernoff
  // bound). Adding 2 additional replicas per block will make the probability even
  // smaller. This test is deterministic, so we expect a failure less often than every 1E3
  // changes to the test, not every 1E3 runs.
  EXPECT_LE(result.MaxNumAssignmentsPerHost(), 140);
  EXPECT_LE(result.MaxNumAssignedBytesPerHost(), 140 * Block::DEFAULT_BLOCK_SIZE);
}

/// Compute a schedule in a split cluster (disjoint set of backends and datanodes).
TEST_F(SchedulerTest, DisjointClusterWithRemoteReads) {
  Cluster cluster;
  for (int i = 0; i < 20; ++i) cluster.AddHost(i < 10, i >= 10);

  Schema schema(cluster);
  schema.AddMultiBlockTable("T1", 10, ReplicaPlacement::REMOTE_ONLY, 3);

  Plan plan(schema);
  plan.AddTableScan("T1");

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  scheduler.Compute(&result);

  EXPECT_EQ(10, result.NumTotalAssignments());
  EXPECT_EQ(10, result.NumRemoteAssignments());
  // Expect that the datanodes were not mistaken for backends.
  for (int i = 10; i < 20; ++i) EXPECT_EQ(0, result.NumTotalAssignments(i));
}

/// Verify that cached replicas take precedence.
TEST_F(SchedulerTest, TestCachedReadPreferred) {
  Cluster cluster;
  cluster.AddHosts(3, true, true);

  Schema schema(cluster);
  schema.AddSingleBlockTable("T1", {0, 2}, {1});

  Plan plan(schema);
  // 1 of the 3 replicas is cached.
  plan.AddTableScan("T1");

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  scheduler.Compute(&result);
  EXPECT_EQ(1 * Block::DEFAULT_BLOCK_SIZE, result.NumCachedAssignedBytes());
  EXPECT_EQ(1 * Block::DEFAULT_BLOCK_SIZE, result.NumCachedAssignedBytes(1));
  EXPECT_EQ(0, result.NumDiskAssignedBytes());
  EXPECT_EQ(0, result.NumRemoteAssignedBytes());

  // Compute additional assignments.
  for (int i = 0; i < 8; ++i) scheduler.Compute(&result);
  EXPECT_EQ(9 * Block::DEFAULT_BLOCK_SIZE, result.NumCachedAssignedBytes());
  EXPECT_EQ(9 * Block::DEFAULT_BLOCK_SIZE, result.NumCachedAssignedBytes(1));
  EXPECT_EQ(0, result.NumDiskAssignedBytes());
  EXPECT_EQ(0, result.NumRemoteAssignedBytes());
}

/// Verify that disable_cached_reads is effective.
TEST_F(SchedulerTest, TestDisableCachedReads) {
  Cluster cluster;
  cluster.AddHosts(3, true, true);

  Schema schema(cluster);
  schema.AddSingleBlockTable("T1", {0, 2}, {1});

  Plan plan(schema);
  // 1 of the 3 replicas is cached.
  plan.AddTableScan("T1");
  plan.SetDisableCachedReads(true);

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  scheduler.Compute(&result);
  EXPECT_EQ(0, result.NumCachedAssignedBytes());
  EXPECT_EQ(1 * Block::DEFAULT_BLOCK_SIZE, result.NumDiskAssignedBytes());
  EXPECT_EQ(0, result.NumRemoteAssignedBytes());

  // Compute additional assignments.
  for (int i = 0; i < 8; ++i) scheduler.Compute(&result);
  EXPECT_EQ(0, result.NumCachedAssignedBytes());
  EXPECT_EQ(9 * Block::DEFAULT_BLOCK_SIZE, result.NumDiskAssignedBytes());
  EXPECT_EQ(0, result.NumRemoteAssignedBytes());
}

/// IMPALA-3019: Test for round robin reset problem. We schedule the same plan twice but
/// send an empty statestored message in between.
/// TODO: This problem cannot occur anymore and the test is merely green for random
/// behavior. Remove.
TEST_F(SchedulerTest, EmptyStatestoreMessage) {
  Cluster cluster;
  cluster.AddHosts(3, false, true);
  cluster.AddHosts(2, true, false);

  Schema schema(cluster);
  schema.AddMultiBlockTable("T1", 1, ReplicaPlacement::RANDOM, 3);

  Plan plan(schema);
  plan.AddTableScan("T1");

  Result result(plan);
  SchedulerWrapper scheduler(plan);

  scheduler.Compute(&result);
  EXPECT_EQ(0, result.NumTotalAssignedBytes(0));
  EXPECT_EQ(0, result.NumTotalAssignedBytes(1));
  EXPECT_EQ(0, result.NumTotalAssignedBytes(2));
  EXPECT_EQ(0, result.NumTotalAssignedBytes(3));
  EXPECT_EQ(1 * Block::DEFAULT_BLOCK_SIZE, result.NumTotalAssignedBytes(4));
  result.Reset();

  scheduler.SendEmptyUpdate();
  scheduler.Compute(&result);
  EXPECT_EQ(0, result.NumTotalAssignedBytes(0));
  EXPECT_EQ(0, result.NumTotalAssignedBytes(1));
  EXPECT_EQ(0, result.NumTotalAssignedBytes(2));
  EXPECT_EQ(1 * Block::DEFAULT_BLOCK_SIZE, result.NumTotalAssignedBytes(3));
  EXPECT_EQ(0, result.NumTotalAssignedBytes(4));
}

/// Test sending updates to the scheduler.
TEST_F(SchedulerTest, TestSendUpdates) {
  Cluster cluster;
  // 3 hosts, only first two run backends. This allows us to remove one of the backends
  // from the scheduler and then verify that reads are assigned to the other backend.
  for (int i=0; i < 3; ++i) cluster.AddHost(i < 2, true);

  Schema schema(cluster);
  schema.AddMultiBlockTable("T1", 1, ReplicaPlacement::RANDOM, 3);

  Plan plan(schema);
  plan.AddTableScan("T1");

  Result result(plan);
  SchedulerWrapper scheduler(plan);

  scheduler.Compute(&result);
  EXPECT_EQ(1 * Block::DEFAULT_BLOCK_SIZE, result.NumDiskAssignedBytes(0));
  EXPECT_EQ(0, result.NumCachedAssignedBytes(0));
  EXPECT_EQ(0, result.NumRemoteAssignedBytes(0));
  EXPECT_EQ(0, result.NumDiskAssignedBytes(1));

  // Remove first host from scheduler.
  scheduler.RemoveBackend(cluster.hosts()[0]);
  result.Reset();

  scheduler.Compute(&result);
  EXPECT_EQ(0, result.NumDiskAssignedBytes(0));
  EXPECT_EQ(1 * Block::DEFAULT_BLOCK_SIZE, result.NumDiskAssignedBytes(1));

  // Re-add first host from scheduler.
  scheduler.AddBackend(cluster.hosts()[0]);
  result.Reset();

  scheduler.Compute(&result);
  EXPECT_EQ(1 * Block::DEFAULT_BLOCK_SIZE, result.NumDiskAssignedBytes(0));
  EXPECT_EQ(0, result.NumDiskAssignedBytes(1));
}

/// IMPALA-4329: Test scheduling with no backends.
TEST_F(SchedulerTest, TestEmptyBackendConfig) {
  Cluster cluster;
  cluster.AddHost(false, true);

  Schema schema(cluster);
  schema.AddMultiBlockTable("T", 1, ReplicaPlacement::REMOTE_ONLY, 1);

  Plan plan(schema);
  plan.AddTableScan("T");

  Result result(plan);
  SchedulerWrapper scheduler(plan);
  Status status = scheduler.Compute(&result);
  EXPECT_TRUE(!status.ok());
  EXPECT_EQ(
      status.GetDetail(), "Cannot schedule query: no registered backends available.\n");
}

}  // end namespace impala

IMPALA_TEST_MAIN();
