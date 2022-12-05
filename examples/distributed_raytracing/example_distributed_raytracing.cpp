/****************************************************************************
 * Copyright (c) 2017-2022 by the ArborX authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the ArborX library. ArborX is                       *
 * distributed under a BSD 3-clause license. For the licensing terms see    *
 * the LICENSE file in the top-level directory.                             *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

/*
 * This example demonstrates how to use ArborX for a raytracing example where
 * rays carry energy that they deposit onto given boxes as they traverse them.
 * The order in which these rays (which originate from one of the boxes) hit
 * the boxes is important in this case, since the ray loses energy on
 * intersection. The example shows three different ways to do that:
 * 1.) using a specialized traversal that orders all intersection in a heap
 *     so that the callbacks for a specific ray are called in the correct order
 *     (OrderedIntersectsBased namespace).
 * 2.) storing all intersections and doing the deposition of energy in a
 *     postprocessing step (IntersectsBased namespace).
 * 3.) using a distributedTree with the reverse Monte-Carlo approach, where
 *     rays are traced from their point of absorption, accumulating intensity
 *     from every cell they intersect, and finally depositing that to the
 *     originating cell.
 *
 */

#include <ArborX.hpp>
#include <ArborX_DetailsKokkosExtArithmeticTraits.hpp>
#include <ArborX_Ray.hpp>

#include <Kokkos_Core.hpp>
#include <Kokkos_Random.hpp>

#include <boost/program_options.hpp>

#include <iostream>
#include <numeric>

#include <mpi.h>

constexpr auto pi = Kokkos::Experimental::pi_v<float>;

// The total energy that is distributed across all rays.
constexpr float temp = 2000.f;   // medium temperature [Kelvin]
constexpr float kappa = 10.f;    // radiative absorption coefficient [1/meters]
constexpr float sigma = 5.67e-8; // stefan-boltzmann constant

// Box Emission [W/m^3]: See "Radiative Heat Transfer" Modest, v.3, ch. 21
// (Monte-Carlo method)
constexpr float box_emission = 4 * kappa * sigma * pow(temp, 4.f);

// Energy a rays loses when passing through a cell.
KOKKOS_INLINE_FUNCTION float lost_energy(float ray_energy, float path_length)
{
#if KOKKOS_VERSION >= 30700
  using Kokkos::expm1;
#else
  using Kokkos::Experimental::expm1;
#endif
  return -ray_energy * expm1(-path_length);
}

namespace IntersectsBased
{

/*
 * Storage for the rays and access traits used in the query/traverse.
 */
template <typename MemorySpace>
struct Rays
{
  Kokkos::View<ArborX::Experimental::Ray *, MemorySpace> _rays;
};

/*
 * IntersectedCell is a storage container for all intersections between rays and
 * boxes that are detected when calling the AccumulateRaySphereIntersections
 * struct. The member variables that are relevant for sorting the intersection
 * according to box and ray are contained in the base class
 * IntersectedCellForSorting as performance improvement.
 */
struct IntersectedCellForSorting
{
  float entrylength;
  int ray_id;
  friend KOKKOS_FUNCTION bool operator<(IntersectedCellForSorting const &l,
                                        IntersectedCellForSorting const &r)
  {
    if (l.ray_id == r.ray_id)
      return l.entrylength < r.entrylength;
    return l.ray_id < r.ray_id;
  }
};

struct IntersectedCell : public IntersectedCellForSorting
{
  float optical_path_length; // optical distance through box
  int cell_id;               // box ID
  KOKKOS_FUNCTION IntersectedCell() = default;
  KOKKOS_FUNCTION IntersectedCell(float entry_length, float path_length,
                                  int primitive_index, int predicate_index)
      : IntersectedCellForSorting{entry_length, predicate_index}
      , optical_path_length(path_length)
      , cell_id(primitive_index)
  {}
};

/*
 * Callback for storing all intersections.
 */
template <typename MemorySpace>
struct AccumulateRaySphereIntersections
{
  Kokkos::View<ArborX::Box *, MemorySpace> _boxes;

  template <typename Predicate, typename OutputFunctor>
  KOKKOS_FUNCTION void operator()(Predicate const &predicate,
                                  int const primitive_index,
                                  OutputFunctor const &out) const
  {
    float length;
    float entrylength;
    auto const &ray = ArborX::getGeometry(predicate);
    auto const &box = _boxes(primitive_index);
    int const predicate_index = ArborX::getData(predicate);
    overlapDistance(ray, box, length, entrylength);
    out(IntersectedCell{/*entrylength*/ entrylength,
                        /*optical_path_length*/ kappa * length,
                        /*cell_id*/ primitive_index,
                        /*ray_id*/ predicate_index});
  }
};

} // namespace IntersectsBased

template <typename MemorySpace>
struct ArborX::AccessTraits<IntersectsBased::Rays<MemorySpace>,
                            ArborX::PredicatesTag>
{
  using memory_space = MemorySpace;
  using size_type = std::size_t;

  KOKKOS_FUNCTION
  static size_type size(IntersectsBased::Rays<MemorySpace> const &rays)
  {
    return rays._rays.extent(0);
  }
  KOKKOS_FUNCTION
  static auto get(IntersectsBased::Rays<MemorySpace> const &rays, size_type i)
  {
    return attach(intersects(rays._rays(i)), (int)i);
  }
};

namespace MPIbased
{

template <typename MemorySpace>
struct Rays
{
  Kokkos::View<ArborX::Experimental::Ray *, MemorySpace> _rays;
};

/*
 * IntersectedRank is similar to IntersectedCell but applies for all
 * intersections between rays and MPI ranks that are detected when calling the
 * AccumulateRayRankIntersections struct. The member variables that are relevant
 * for sorting the intersection according to rank and ray are contained in the
 * base class IntersectedRankForSorting as performance improvement.
 */
struct IntersectedRankForSorting
{
  float entrylength;
  int ray_id;
  friend KOKKOS_FUNCTION bool operator<(IntersectedRankForSorting const &l,
                                        IntersectedRankForSorting const &r)
  {
    if (l.ray_id == r.ray_id)
      return l.entrylength < r.entrylength;
    return l.ray_id < r.ray_id;
  }
};

struct IntersectedRank : public IntersectedRankForSorting
{
  float optical_path_length;    // optical distance through rank
  float intensity_contribution; // contribution of rank to ray intensity
  KOKKOS_FUNCTION IntersectedRank() = default;
  KOKKOS_FUNCTION IntersectedRank(float entry_length, float path_length,
                                  float rank_intensity_contribution,
                                  int predicate_index)
      : IntersectedRankForSorting{entry_length, predicate_index}
      , optical_path_length(path_length)
      , intensity_contribution(rank_intensity_contribution)
  {}
};

/*
 *  Callback for storing accumulated optical distances
 *  through ranks (sum of distances traversed through all cells
 *  in rank multiplied by each cell's absorption coefficient) and
 *  accumulated contribution of rank to ray intensity.
 *
 */
template <typename MemorySpace>
struct AccumulateRayRankIntersections
{
  using tag = ArborX::Details::PostCallbackTag;
  Kokkos::View<ArborX::Box *, MemorySpace> boxes;
  int rank;

  template <typename Predicates, typename InOutView, typename InView,
            typename OutView>
  void operator()(Predicates const &queries, InOutView &offset, InView &in,
                  OutView &out) const
  {
    auto const n = offset.extent(0) - 1;
    auto const nrays = queries.extent(0);
    auto const nintersects = in.extent(0);
    auto const &boxes_ = boxes; // why do we do this?
    Kokkos::realloc(out, n);    // one for each ray
    constexpr auto inf = KokkosExt::ArithmeticTraits::infinity<float>::value;

    /* Because the rank's cells are not defined in order of intersection
     * within the "in" view, then we must first iterate through each ray-cell
     * intersection to determine the entrancelength and optical distance,
     * then sort by entrancelength, then iterate through to determine the
     * radiation intensity contributions.
     *
     * An ordered intersects traversal of the bottom_tree would be better
     * I think
     */
    Kokkos::View<float *, MemorySpace> optical_distances(
        Kokkos::view_alloc("Example::optical_distances",
                           Kokkos::WithoutInitializing),
        nintersects);
    Kokkos::View<IntersectsBased::IntersectedCellForSorting *, MemorySpace>
        sort_array(Kokkos::view_alloc("Example::IntersectedCells",
                                      Kokkos::WithoutInitializing),
                   nintersects);
    Kokkos::parallel_for(
        "Evaluating ray-cell interaction", nrays, KOKKOS_LAMBDA(int i) {
          auto const &ray = ArborX::getGeometry(queries(i));
          float length, entrylength;
          for (int j = offset(i); j < offset(i + 1); ++j)
          {
            auto const &box = boxes_(in(j));
            overlapDistance(ray, box, length, entrylength);
            optical_distances(j) = length * kappa;
            sort_array(j) =
                IntersectsBased::IntersectedCellForSorting{entrylength, i};
          }
        });

    // Sorting cell intersections within rank (TODO OrderedIntersects for this?)
    Kokkos::Profiling::pushRegion("Example::sorting by key");
#if defined(KOKKOS_ENABLE_CUDA) || defined(KOKKOS_ENABLE_HIP) ||               \
    defined(KOKKOS_ENABLE_SYCL)
    auto permutation = ArborX::Details::sortObjects(exec_space, sort_array);
#else
    auto sort_array_host =
        Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, sort_array);
    Kokkos::View<int *, Kokkos::HostSpace> permutation_host(
        Kokkos::view_alloc("Example::permutation", Kokkos::WithoutInitializing),
        sort_array.size());
    std::iota(permutation_host.data(),
              permutation_host.data() + sort_array_host.size(), 0);
    std::sort(permutation_host.data(),
              permutation_host.data() + sort_array_host.size(),
              [&](int const &a, int const &b) {
                return (sort_array_host(a) < sort_array_host(b));
              });
    auto permutation =
        Kokkos::create_mirror_view_and_copy(MemorySpace{}, permutation_host);
#endif
    Kokkos::Profiling::popRegion();

    // Accumulating two ouputted values for this rank
    Kokkos::parallel_for(
        "Evaluating ray-cell interaction", nrays, KOKKOS_LAMBDA(int i) {
#if KOKKOS_VERSION >= 30700
          using Kokkos::exp;
          using Kokkos::pow;
#else
        using Kokkos::Experimental::exp;
        using Kokkos::Experimental::pow;
#endif
          float accum_optical_length = 0., rankentrylength = inf;
          float intensity_contribution = 0., optical_length_in;
          auto const &ray = ArborX::getGeometry(queries(i));

          // Iterating through rank's boxes in order of intersection
          for (int j = offset(i); j < offset(i + 1); ++j)
          {
            auto const &box = boxes_(in(permutation(j)));
            optical_length_in = accum_optical_length;
            accum_optical_length += optical_distances(permutation(j));
            intensity_contribution +=
                sigma * pow(temp, 4.f) / pi *
                (exp(-optical_length_in) - exp(-accum_optical_length));
          }

          // Rank entrance length
          rankentrylength = sort_array(permutation(offset(i))).entrylength;

          // Rank output data structure
          out(i) = IntersectedRank{
              /*entrylength*/ rankentrylength,
              /*optical_path_length*/ accum_optical_length,
              /*intensity contr*/ intensity_contribution,
              /*ray_id*/ 0}; // ray ID on originating rank is tracked by
                             // distributedTree and will be applied later for
                             // sorting

          // offset values reset to reflect only one output per
          // ray/rank intersection
          // required for communicateResultsBack to work
          offset(i) = i;
          if (i == n - 1)
            offset(n) = n; // set last value as well
        });
  }
};

} // namespace MPIbased

template <typename MemorySpace>
struct ArborX::AccessTraits<MPIbased::Rays<MemorySpace>, ArborX::PredicatesTag>
{
  using memory_space = MemorySpace;
  using size_type = std::size_t;

  KOKKOS_FUNCTION
  static size_type size(MPIbased::Rays<MemorySpace> const &rays)
  {
    return rays._rays.extent(0);
  }
  KOKKOS_FUNCTION
  static auto get(MPIbased::Rays<MemorySpace> const &rays, size_type i)
  {
    return attach(intersects(rays._rays(i)), (int)i);
  }
};

template <typename View, typename Boxes>
void printoutput(View &energies, Boxes &boxes, float dx, float dy, float dz)
{
  for (int i = 0; i < boxes.extent(0); i++)
  {
    auto const &box = boxes(i);
    printf("%10d %20.5f %20.5f %20.5f %20.5f\n", i,
           (box.minCorner()[0] + box.maxCorner()[0]) / 2,
           (box.minCorner()[1] + box.maxCorner()[1]) / 2,
           (box.minCorner()[2] + box.maxCorner()[2]) / 2,
           energies(i) / (dx * dy * dz));
  }
}

int main(int argc, char *argv[])
{
  using ExecutionSpace = Kokkos::DefaultExecutionSpace;
  using MemorySpace = ExecutionSpace::memory_space;

  Kokkos::ScopeGuard guard(argc, argv);

  namespace bpo = boost::program_options;

  int nx;
  int ny;
  int nz;
  int rays_per_box;
  float lx;
  float ly;
  float lz;
  bool parallel;
  bool print;

  bpo::options_description desc("Allowed options");
  // clang-format off
  desc.add_options()
    ("help", "help message" )
    ("rays per box", bpo::value<int>(&rays_per_box)->default_value(10), 
     "number of rays") 
    ("lx", bpo::value<float>(&lx)->default_value(100.0), "Length of X side")
    ("ly", bpo::value<float>(&ly)->default_value(100.0), "Length of Y side")
    ("lz", bpo::value<float>(&lz)->default_value(100.0), "Length of Z side")
    ("nx", bpo::value<int>(&nx)->default_value(10), "number of X boxes")
    ("ny", bpo::value<int>(&ny)->default_value(10), "number of Y boxes")
    ("nz", bpo::value<int>(&nz)->default_value(10), "number of Z boxes")
    ("parallel", bpo::value<bool>(&parallel)->default_value(false),
     "run with MPI (true/false)")
    ("print", bpo::value<bool>(&print)->default_value(false), "Print output")
    ;
  // clang-format on
  bpo::variables_map vm;
  bpo::store(bpo::command_line_parser(argc, argv).options(desc).run(), vm);
  bpo::notify(vm);

  if (vm.count("help") > 0)
  {
    std::cout << desc << '\n';
    return 1;
  }

  int num_boxes = nx * ny * nz;
  float dx = lx / (float)nx;
  float dy = ly / (float)ny;
  float dz = lz / (float)nz;

  ExecutionSpace exec_space{};

  Kokkos::Profiling::pushRegion("Example::problem_setup");
  Kokkos::Profiling::pushRegion("Example::make_grid");
  Kokkos::View<ArborX::Box *, MemorySpace> boxes(
      Kokkos::view_alloc(exec_space, Kokkos::WithoutInitializing,
                         "Example::boxes"),
      num_boxes);
  Kokkos::parallel_for(
      "Example::initialize_boxes",
      Kokkos::MDRangePolicy<Kokkos::Rank<3>, ExecutionSpace>(
          exec_space, {0, 0, 0}, {nx, ny, nz}),
      KOKKOS_LAMBDA(int i, int j, int k) {
        int const box_id = i + nx * j + nx * ny * k;
        boxes(box_id) = {{i * dx, j * dy, k * dz},
                         {(i + 1) * dx, (j + 1) * dy, (k + 1) * dz}};
      });
  Kokkos::Profiling::popRegion();

  // For every box shoot rays from random (uniformly distributed) points inside
  // the box in random (uniformly distributed) directions.
  Kokkos::Profiling::pushRegion("Example::make_rays");
  Kokkos::View<ArborX::Experimental::Ray *, MemorySpace> rays(
      Kokkos::view_alloc(Kokkos::WithoutInitializing, "Example::rays"),
      rays_per_box * num_boxes);
  {
    using RandPoolType = Kokkos::Random_XorShift64_Pool<>;
    RandPoolType rand_pool(5374857);
    using GeneratorType = RandPoolType::generator_type;

    Kokkos::parallel_for(
        "Example::initialize_rays",
        Kokkos::MDRangePolicy<Kokkos::Rank<2>, ExecutionSpace>(
            exec_space, {0, 0}, {num_boxes, rays_per_box}),
        KOKKOS_LAMBDA(const size_t i, const size_t j) {
          // The origins of rays are uniformly distributed in the boxes. The
          // direction vectors are uniformly sampling of a full sphere.
          GeneratorType g = rand_pool.get_state();
#if KOKKOS_VERSION >= 30700
          using Kokkos::cos;
          using Kokkos::sin;
          using Kokkos::acos;
#else
          using Kokkos::Experimental::cos;
          using Kokkos::Experimental::sin;
          using Kokkos::Experimental::acos;
#endif

          ArborX::Box const &b = boxes(i);
          ArborX::Point origin{
              b.minCorner()[0] +
                  Kokkos::rand<GeneratorType, float>::draw(g, dx),
              b.minCorner()[1] +
                  Kokkos::rand<GeneratorType, float>::draw(g, dy),
              b.minCorner()[2] +
                  Kokkos::rand<GeneratorType, float>::draw(g, dz)};

          float upsilon =
              Kokkos::rand<GeneratorType, float>::draw(g, 2.f * M_PI);
          float theta =
              acos(1 - 2 * Kokkos::rand<GeneratorType, float>::draw(g));
          ArborX::Experimental::Vector direction{
              cos(upsilon) * sin(theta), sin(upsilon) * sin(theta), cos(theta)};

          rays(j + i * rays_per_box) =
              ArborX::Experimental::Ray{origin, direction};

          rand_pool.free_state(g);
        });
  }
  Kokkos::Profiling::popRegion();
  Kokkos::Profiling::popRegion();

  // Distributed raytracing approach
  Kokkos::View<float *, MemorySpace> energy_distributed_intersects;
  int rank; // needed for post-processing
    MPI_Init(NULL, NULL);
    {
      int nranks, num_boxes_per_rank;
      MPI_Comm_size(MPI_COMM_WORLD, &nranks);
      MPI_Comm_rank(MPI_COMM_WORLD, &rank);
      if (rank == 0)
      {
        std::cout << "Running with " << nranks << " MPI ranks" << std::endl;
        if (num_boxes % nranks != 0)
          std::cerr << "WARNING: Number of boxes (" << num_boxes
                    << ") indivisible by number of ranks (" << nranks << ")"
                    << std::endl;
      }
      num_boxes_per_rank = num_boxes / nranks;

      // Distributed BVH: This rank gets only a subsect of the overall boxes
      // TODO: avoid unecessarily duplicating all of the boxes for all of the
      // ranks
      auto boxes_for_rank = Kokkos::subview(
          boxes, std::pair<int, int>(num_boxes_per_rank * rank,
                                     num_boxes_per_rank * (rank + 1)));
      ArborX::DistributedTree<MemorySpace> distributed_bvh{
          MPI_COMM_WORLD, exec_space, boxes_for_rank};

      // Init Rays
      // TODO: avoid unecessary duplication, as above
      auto rays_for_rank = Kokkos::subview(
          rays,
          std::pair<int, int>(num_boxes_per_rank * rays_per_box * rank,
                              num_boxes_per_rank * rays_per_box * (rank + 1)));

      // TODO: is there ordered traversal with distributedTree?
      Kokkos::View<MPIbased::IntersectedRank *, MemorySpace> values("values",
                                                                    0);
      Kokkos::View<int *, MemorySpace> offsets("offsets", 0);
      distributed_bvh.query(
          exec_space, MPIbased::Rays<MemorySpace>{rays_for_rank},
          MPIbased::AccumulateRayRankIntersections<MemorySpace>{boxes_for_rank,
                                                                rank},
          values, offsets);

      // Ray IDs from originating rank need to be applied for sorting
      Kokkos::parallel_for(
          "Applying ray IDs", rays_for_rank.extent(0),
          KOKKOS_LAMBDA(int const i) {
            for (int j = offsets(i); j < offsets(i + 1); j++)
            {
              values(j).ray_id = i;
            }
          });

      // Sorting ranks by intersection length
      Kokkos::View<MPIbased::IntersectedRankForSorting *, MemorySpace>
          sort_array(Kokkos::view_alloc(exec_space, Kokkos::WithoutInitializing,
                                        "Example::sort_array"),
                     values.size());
      Kokkos::parallel_for(
          "Example::copy sort_array",
          Kokkos::RangePolicy<ExecutionSpace>(exec_space, 0, values.size()),
          KOKKOS_LAMBDA(int i) { sort_array(i) = values(i); });
#if defined(KOKKOS_ENABLE_CUDA) || defined(KOKKOS_ENABLE_HIP) ||               \
    defined(KOKKOS_ENABLE_SYCL)
      auto permutation = ArborX::Details::sortObjects(exec_space, sort_array);
#else
      auto sort_array_host =
          Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, sort_array);
      Kokkos::View<int *, Kokkos::HostSpace> permutation_host(
          Kokkos::view_alloc("Example::permutation",
                             Kokkos::WithoutInitializing),
          sort_array_host.size());
      std::iota(permutation_host.data(),
                permutation_host.data() + sort_array_host.size(), 0);
      std::sort(permutation_host.data(),
                permutation_host.data() + sort_array_host.size(),
                [&](int const &a, int const &b) {
                  return (sort_array_host(a) < sort_array_host(b));
                });
      auto permutation =
          Kokkos::create_mirror_view_and_copy(MemorySpace{}, permutation_host);
#endif
      energy_distributed_intersects = Kokkos::View<float *, MemorySpace>(
          "Example::energy_distributed_intersects", num_boxes);

      Kokkos::parallel_for(
          "Evaluating ray intensities", rays_for_rank.extent(0),
          KOKKOS_LAMBDA(int const i) {
#if KOKKOS_VERSION >= 30700
            using Kokkos::exp;
#else
            using Kokkos::Experimental::exp;
#endif
            float accum_opt_dist = 0, ray_intensity = 0;
            for (int j = offsets(i); j < offsets(i + 1); ++j)
            { // each intersected rank
              const auto &v = values(permutation(j));
              ray_intensity += exp(-accum_opt_dist) * v.intensity_contribution;
              accum_opt_dist += v.optical_path_length;
            }
            int startbox = num_boxes_per_rank * rank;   // for this rank
            int bid = (int)i / rays_per_box + startbox; // global box ID
            Kokkos::atomic_add(&energy_distributed_intersects(bid),
                               ray_intensity * 4 * pi * kappa / rays_per_box);
          });

      // Combine all results to the root
      MPI_Reduce(rank == 0 ? MPI_IN_PLACE
                           : energy_distributed_intersects.data(),
                 energy_distributed_intersects.data(), num_boxes, MPI_FLOAT,
                 MPI_SUM, 0, MPI_COMM_WORLD);
      if (rank == 0 and print)
      {
        printf("Net radiative absorptions:\n");
        printoutput(energy_distributed_intersects, boxes, 1, 1, 1);
        printf("\n\n");
      }
    }
    MPI_Finalize();

  return EXIT_SUCCESS;
}