/***************************************************************************
 *  Copyright (C) 2014 Sebastian Schlag <sebastian.schlag@kit.edu>
 **************************************************************************/

#include "partition/Partitioner.h"

#include "lib/definitions.h"
#include "lib/io/HypergraphIO.h"
#include "lib/io/PartitioningOutput.h"
#include "partition/Configuration.h"
#include "partition/refinement/TwoWayFMRefiner.h"
#include "tools/RandomFunctions.h"

#ifndef NDEBUG
#include "partition/Metrics.h"
#endif

using defs::HighResClockTimepoint;

namespace partition {
void Partitioner::partition(Hypergraph& hypergraph, ICoarsener& coarsener,
                            IRefiner& refiner) {
  std::vector<HyperedgeID> removed_hyperedges;
  removeLargeHyperedges(hypergraph, removed_hyperedges);

#ifndef NDEBUG
  HyperedgeWeight initial_cut = std::numeric_limits<HyperedgeWeight>::max();
#endif

  HighResClockTimepoint start;
  HighResClockTimepoint end;

  for (int vcycle = 0; vcycle < _config.partition.global_search_iterations; ++vcycle) {
    start = std::chrono::high_resolution_clock::now();
    coarsener.coarsen(_config.coarsening.minimal_node_count);
    end = std::chrono::high_resolution_clock::now();
    _timings[kCoarsening] += end - start;

    if (vcycle == 0) {
      start = std::chrono::high_resolution_clock::now();
      performInitialPartitioning(hypergraph);
      end = std::chrono::high_resolution_clock::now();
      _timings[kInitialPartitioning] = end - start;
    }

    start = std::chrono::high_resolution_clock::now();
    coarsener.uncoarsen(refiner);
    end = std::chrono::high_resolution_clock::now();
    _timings[kUncoarseningRefinement] += end - start;
    DBG(dbg_partition_vcycles, "vcycle # " << vcycle << ": cut=" << metrics::hyperedgeCut(hypergraph));
    ASSERT(metrics::hyperedgeCut(hypergraph) <= initial_cut, "Uncoarsening worsened cut:"
           << metrics::hyperedgeCut(hypergraph) << ">" << initial_cut);
    ++_config.partition.current_v_cycle;
#ifndef NDEBUG
    initial_cut = metrics::hyperedgeCut(hypergraph);
#endif
  }

  restoreLargeHyperedges(hypergraph, removed_hyperedges);
}

void Partitioner::removeLargeHyperedges(Hypergraph& hg, std::vector<HyperedgeID>& removed_hyperedges) {
  if (_config.partition.hyperedge_size_threshold != -1) {
    for (auto && he : hg.edges()) {
      if (hg.edgeSize(he) > _config.partition.hyperedge_size_threshold) {
        DBG(dbg_partition_large_he_removal, "Hyperedge " << he << ": size ("
            << hg.edgeSize(he) << ")   exceeds threshold: "
            << _config.partition.hyperedge_size_threshold);
        removed_hyperedges.push_back(he);
        hg.removeEdge(he, true);
      }
    }
  }
}

void Partitioner::restoreLargeHyperedges(Hypergraph& hg, std::vector<HyperedgeID>& removed_hyperedges) {
  if (_config.partition.hyperedge_size_threshold != -1) {
    PartitionWeights partition_weights(_config.partition.k, 0);
    for (auto && hn : hg.nodes()) {
      if (hg.partID(hn) != Hypergraph::kInvalidPartition) {
        partition_weights[hg.partID(hn)] += hg.nodeWeight(hn);
      }
    }

    for (auto && edge = removed_hyperedges.rbegin(); edge != removed_hyperedges.rend(); ++edge) {
      DBG(dbg_partition_large_he_removal, " restore Hyperedge " << *edge);
      hg.restoreEdge(*edge);
      partitionUnpartitionedPins(*edge, hg, partition_weights);
    }
    ASSERT(metrics::imbalance(hg) <= _config.partition.epsilon,
           "Final assignment of unpartitioned pins violated balance constraint");
  }
}

void Partitioner::partitionUnpartitionedPins(HyperedgeID he, Hypergraph& hg,
                                             PartitionWeights& partition_weights) {
  HypernodeID num_pins = hg.edgeSize(he);
  HypernodeID num_unpartitioned_hns = 0;
  HypernodeWeight unpartitioned_weight = 0;
  for (auto && pin : hg.pins(he)) {
    if (hg.partID(pin) == Hypergraph::kInvalidPartition) {
      ++num_unpartitioned_hns;
      unpartitioned_weight += hg.nodeWeight(pin);
    }
  }

  if (num_unpartitioned_hns == num_pins) {
    if (partition_weights[0] + unpartitioned_weight
        <= _config.partition.max_part_size) {
      assignAllPinsToPartition(he, 0, hg, partition_weights);
    } else if (partition_weights[1] + unpartitioned_weight
               <= _config.partition.max_part_size) {
      assignAllPinsToPartition(he, 1, hg, partition_weights);
    }
    return;
  }
  if ((hg.pinCountInPart(he, 0) > 0 && hg.pinCountInPart(he, 1) == 0) &&
      (partition_weights[0] + unpartitioned_weight
       <= _config.partition.max_part_size)) {
    assignUnpartitionedPinsToPartition(he, 0, hg, partition_weights);
    return;
  }
  if ((hg.pinCountInPart(he, 1) > 0 && hg.pinCountInPart(he, 0) == 0) &&
      (partition_weights[1] + unpartitioned_weight
       <= _config.partition.max_part_size)) {
    assignUnpartitionedPinsToPartition(he, 1, hg, partition_weights);
    return;
  }
  distributePinsAcrossPartitions(he, hg, partition_weights);
}

void Partitioner::assignUnpartitionedPinsToPartition(HyperedgeID he, PartitionID id, Hypergraph& hg,
                                                     PartitionWeights& partition_weights) {
  DBG(dbg_partition_large_he_removal,
      "Assigning unpartitioned pins of HE " << he << " to partition " << id);
  for (auto && pin : hg.pins(he)) {
    ASSERT(hg.partID(pin) == Hypergraph::kInvalidPartition || hg.partID(pin) == id,
           "HN " << pin << " is not in partition " << id << " but in "
           << hg.partID(pin));
    if (hg.partID(pin) == Hypergraph::kInvalidPartition) {
      hg.setNodePart(pin, id);
      partition_weights[id] += hg.nodeWeight(pin);
    }
  }
}

void Partitioner::assignAllPinsToPartition(HyperedgeID he, PartitionID id, Hypergraph& hg, PartitionWeights&
                                           partition_weights) {
  DBG(dbg_partition_large_he_removal, "Assigning all pins of HE " << he << " to partition " << id);
  for (auto && pin : hg.pins(he)) {
    ASSERT(hg.partID(pin) == Hypergraph::kInvalidPartition,
           "HN " << pin << " is not in partition " << id << " but in "
           << hg.partID(pin));
    hg.setNodePart(pin, id);
    partition_weights[id] += hg.nodeWeight(pin);
  }
}

void Partitioner::distributePinsAcrossPartitions(HyperedgeID he, Hypergraph& hg,
                                                 PartitionWeights& partition_weights) {
  DBG(dbg_partition_large_he_removal, "Distributing pins of HE " << he << " to both partitions");
  size_t min_partition = 0;
  for (auto && pin : hg.pins(he)) {
    if (hg.partID(pin) == Hypergraph::kInvalidPartition) {
      min_partition = std::min_element(partition_weights.begin(), partition_weights.end())
                      - partition_weights.begin();
      hg.setNodePart(pin, min_partition);
      partition_weights[min_partition] += hg.nodeWeight(pin);
    }
  }
}

void Partitioner::createMappingsForInitialPartitioning(HmetisToCoarsenedMapping& hmetis_to_hg,
                                                       CoarsenedToHmetisMapping& hg_to_hmetis,
                                                       const Hypergraph& hg) {
  int i = 0;
  for (auto && hn : hg.nodes()) {
    hg_to_hmetis[hn] = i;
    hmetis_to_hg[i] = hn;
    ++i;
  }
}

void Partitioner::performInitialPartitioning(Hypergraph& hg) {
  io::printHypergraphInfo(hg, _config.partition.coarse_graph_filename.substr(
                            _config.partition.coarse_graph_filename.find_last_of("/") + 1));

  DBG(dbg_partition_initial_partitioning, "# unconnected hypernodes = "
      << std::to_string([&]() {
                          HypernodeID count = 0;
                          for (auto && hn : hg.nodes()) {
                            if (hg.nodeDegree(hn) == 0) {
                              ++count;
                            }
                          }
                          return count;
                        } ()));

  HmetisToCoarsenedMapping hmetis_to_hg(hg.numNodes(), 0);
  CoarsenedToHmetisMapping hg_to_hmetis;
  createMappingsForInitialPartitioning(hmetis_to_hg, hg_to_hmetis, hg);

  io::writeHypergraphForhMetisPartitioning(hg, _config.partition.coarse_graph_filename,
                                           hg_to_hmetis);

  std::vector<PartitionID> partitioning;
  std::vector<PartitionID> best_partitioning;
  partitioning.reserve(hg.numNodes());
  best_partitioning.reserve(hg.numNodes());

  HyperedgeWeight best_cut = std::numeric_limits<HyperedgeWeight>::max();
  HyperedgeWeight current_cut = std::numeric_limits<HyperedgeWeight>::max();

  // We use hMetis-RB as initial partitioner. If called to partition a graph into k parts
  // with an UBfactor of b, the maximal allowed partition size will be 0.5+(b/100)^(log2(k)) n.
  // In order to provide a balanced initial partitioning, we determine the UBfactor such that
  // the maximal allowed partiton size corresponds to our upper bound i.e.
  // (1+epsilon) * ceil(total_weight / k).
  double exp = 1.0 / log2(_config.partition.k);
  double ub_factor = 50.0 * (2 * pow((1 + _config.partition.epsilon), exp)
                             * pow(ceil(hg.initialNumNodes() / _config.partition.k)
                                   / hg.initialNumNodes(), exp) - 1);

  for (int attempt = 0; attempt < _config.partition.initial_partitioning_attempts; ++attempt) {
    int seed = Randomize::newRandomSeed();
    std::string hmetis_call("/home/schlag/hmetis-2.0pre1/Linux-x86_64/hmetis2.0pre1 "
                            + _config.partition.coarse_graph_filename
                            + " " + std::to_string(_config.partition.k)
                            + " -seed=" + std::to_string(seed)
                            + " -ufactor=" + std::to_string(ub_factor)
                            + (_config.partition.verbose_output ? "" : " > /dev/null"));
    LOG(hmetis_call);
    std::system(hmetis_call.c_str());

    io::readPartitionFile(_config.partition.coarse_graph_partition_filename, partitioning);
    ASSERT(partitioning.size() == hg.numNodes(), "Partition file has incorrect size");

    current_cut = metrics::hyperedgeCut(hg, hg_to_hmetis, partitioning);
    DBG(dbg_partition_initial_partitioning, "attempt " << attempt << " seed("
        << seed << "):" << current_cut << " - balance=" << metrics::imbalance(hg, hg_to_hmetis, partitioning));
    if (current_cut < best_cut) {
      DBG(dbg_partition_initial_partitioning, "Attempt " << attempt
          << " improved initial cut from " << best_cut << " to " << current_cut);
      best_partitioning.swap(partitioning);
      best_cut = current_cut;
    }
    partitioning.clear();
  }

  ASSERT(best_cut != std::numeric_limits<HyperedgeWeight>::max(), "No min cut calculated");
  for (size_t i = 0; i < best_partitioning.size(); ++i) {
    hg.setNodePart(hmetis_to_hg[i], best_partitioning[i]);
  }
  ASSERT(metrics::hyperedgeCut(hg) == best_cut, "Cut induced by hypergraph does not equal "
         << "best initial cut");
}
} // namespace partition
