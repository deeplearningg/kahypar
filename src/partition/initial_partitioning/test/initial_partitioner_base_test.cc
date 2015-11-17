/*
 * initial_partitioner_base_test.cc
 *
 *  Created on: Apr 17, 2015
 *      Author: theuer
 */

#include "gmock/gmock.h"

#include "partition/initial_partitioning/InitialPartitionerBase.h"

using::testing::Eq;
using::testing::Test;

using defs::Hypergraph;
using defs::HyperedgeIndexVector;
using defs::HyperedgeVector;
using defs::HyperedgeID;

namespace partition {
class InitialPartitionerBaseTest : public Test {
 public:
  InitialPartitionerBaseTest() :
    hypergraph(7, 4,
               HyperedgeIndexVector { 0, 2, 6, 9,  /*sentinel*/ 12 },
               HyperedgeVector { 0, 2, 0, 1, 3, 4, 3, 4, 6, 2, 5, 6 }), config(), partitioner(
      nullptr) {
    HypernodeWeight hypergraph_weight = 0;
    for (HypernodeID hn : hypergraph.nodes()) {
      hypergraph_weight += hypergraph.nodeWeight(hn);
    }

    initializeConfiguration(hypergraph_weight);
    partitioner = new InitialPartitionerBase(hypergraph, config);
    partitioner->recalculateBalanceConstraints(config.initial_partitioning.epsilon);
  }

  void initializeConfiguration(HypernodeWeight hypergraph_weight) {
    config.initial_partitioning.k = 2;
    config.partition.k = 2;
    config.initial_partitioning.epsilon = 0.05;
    config.partition.epsilon = 0.05;
    config.initial_partitioning.seed = 1;
    config.initial_partitioning.rollback = true;
    config.initial_partitioning.upper_allowed_partition_weight.resize(2);
    config.initial_partitioning.perfect_balance_partition_weight.resize(2);
    for (PartitionID i = 0; i < config.initial_partitioning.k; i++) {
      config.initial_partitioning.perfect_balance_partition_weight[i] =
        ceil(
          hypergraph_weight
          / static_cast<double>(config.initial_partitioning.k));
    }
  }

  InitialPartitionerBase* partitioner;
  Hypergraph hypergraph;
  Configuration config;
};

TEST_F(InitialPartitionerBaseTest, AssignHypernodesToPartition) {
  // Assign hypernodes
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(0, 0));
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(1, 0));
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(2, 0));
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(3, 0));
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(4, 1));
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(5, 1));
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(6, 1));

  // Check, if all hypernodes are assigned correctly
  ASSERT_EQ(hypergraph.partID(0), 0);
  ASSERT_EQ(hypergraph.partID(1), 0);
  ASSERT_EQ(hypergraph.partID(2), 0);
  ASSERT_EQ(hypergraph.partID(3), 0);
  ASSERT_EQ(hypergraph.partID(4), 1);
  ASSERT_EQ(hypergraph.partID(5), 1);
  ASSERT_EQ(hypergraph.partID(6), 1);

  hypergraph.initializeNumCutHyperedges();
  // Changing hypernode partition id
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(3, 1));
  ASSERT_FALSE(partitioner->assignHypernodeToPartition(3, 1));
}

TEST_F(InitialPartitionerBaseTest, CheckHyperedgeCutAfterRollbackToBestCut) {
  // Assign hypernodes
  hypergraph.setNodePart(0, 1);
  hypergraph.setNodePart(1, 1);
  hypergraph.setNodePart(2, 1);
  hypergraph.setNodePart(3, 1);
  hypergraph.setNodePart(4, 1);
  hypergraph.setNodePart(5, 1);
  hypergraph.setNodePart(6, 1);
  hypergraph.initializeNumCutHyperedges();
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(0, 0));
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(1, 0));
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(2, 0));
  ASSERT_TRUE(partitioner->assignHypernodeToPartition(3, 0));

  // Perform rollback to best bisection cut
  HyperedgeWeight cut_before = metrics::hyperedgeCut(hypergraph);
  partitioner->rollbackToBestCut();
  HyperedgeWeight cut_after = metrics::hyperedgeCut(hypergraph);

  // The best bisection cut is 2 (0,1,2 -> 0 and 3,4,5,6 -> 1)
  ASSERT_TRUE(cut_after < cut_before);
  ASSERT_EQ(cut_after, 2);
  for (HypernodeID hn : hypergraph.nodes()) {
    if (hn <= 2)
      ASSERT_EQ(hypergraph.partID(hn), 0);
    else
      ASSERT_EQ(hypergraph.partID(hn), 1);
  }
}


TEST_F(InitialPartitionerBaseTest, ResetPartitionToMinusOne) {
  hypergraph.setNodePart(0, 1);
  hypergraph.setNodePart(1, 1);
  hypergraph.setNodePart(2, 1);
  hypergraph.setNodePart(3, 1);
  hypergraph.setNodePart(4, 1);
  hypergraph.setNodePart(5, 1);
  hypergraph.setNodePart(6, 1);

  config.initial_partitioning.unassigned_part = -1;
  partitioner->resetPartitioning();
  for (HypernodeID hn : hypergraph.nodes()) {
    ASSERT_EQ(hypergraph.partID(hn), -1);
  }
}

TEST_F(InitialPartitionerBaseTest, ResetPartitionToPartitionOne) {
  hypergraph.setNodePart(0, 1);
  hypergraph.setNodePart(1, 1);
  hypergraph.setNodePart(2, 1);
  hypergraph.setNodePart(3, 1);
  hypergraph.setNodePart(4, 1);
  hypergraph.setNodePart(5, 1);
  hypergraph.setNodePart(6, 1);

  config.initial_partitioning.unassigned_part = 0;
  partitioner->resetPartitioning();
  for (HypernodeID hn : hypergraph.nodes()) {
    ASSERT_EQ(hypergraph.partID(hn), 0);
  }
}
}
