#include <map_merge_3d/features.h>
#include <map_merge_3d/graph.h>
#include <map_merge_3d/map_merging.h>

#include <pcl/common/transforms.h>

/**
 * @brief Finds transformation between from and to in pairwise_transforms
 * @details May return either transform present in pairwise_transforms or
 * inverse of suitable transform that represent transform between from and to
 * nodes.
 *
 * @param pairwise_transforms transform to look
 * @param from source index
 * @param to target index
 * @return Required transform or zero matrix if the transform could not be
 * found.
 */
static inline Eigen::Matrix4f
getTransform(const std::vector<TransformEstimate> &pairwise_transforms,
             size_t from, size_t to)
{
  for (const auto &est : pairwise_transforms) {
    if (est.source_idx == from && est.target_idx == to) {
      return est.transform.inverse();
    }
    if (est.source_idx == to && est.target_idx == from) {
      return est.transform;
    }
  }

  return Eigen::Matrix4f::Zero();
}

static inline std::vector<Eigen::Matrix4f> computeGlobalTransforms(
    const std::vector<TransformEstimate> &pairwise_transforms,
    double confidence_threshold)
{
  // consider only largest conncted component
  std::vector<TransformEstimate> component =
      largestConnectedComponent(pairwise_transforms, confidence_threshold);

  // find maximum spanning tree
  Graph span_tree;
  std::vector<size_t> span_tree_centers;
  // uses number of inliers as weights
  findMaxSpanningTree(component, span_tree, span_tree_centers);

  // size of the largest connected component
  const size_t nodes_count = numberOfNodesInEstimates(pairwise_transforms);
  // index of the node taken as the reference frame
  const size_t reference_frame = span_tree_centers[0];
  // init all transforms as invalid
  std::vector<Eigen::Matrix4f> global_transforms(nodes_count,
                                                 Eigen::Matrix4f::Zero());
  // refence frame always has identity transform
  global_transforms[reference_frame] = Eigen::Matrix4f::Identity();
  // compute global transforms by chaining them together
  span_tree.walkBreadthFirst(
      span_tree_centers[0],
      [&global_transforms, &component](const GraphEdge &edge) {
        global_transforms[edge.to] =
            global_transforms[edge.from] *
            getTransform(component, edge.from, edge.to);
      });

  return global_transforms;
}

std::vector<Eigen::Matrix4f> estimateMapsTransforms(
    const std::vector<PointCloudPtr> &clouds, const MapMergingParams &params)
{
  // per cloud data extracted for transform estimation
  std::vector<PointCloudPtr> clouds_resized;
  std::vector<SurfaceNormalsPtr> normals;
  std::vector<PointCloudPtr> keypoints;
  std::vector<LocalDescriptorsPtr> descriptors;
  clouds_resized.reserve(clouds.size());
  normals.reserve(clouds.size());
  keypoints.reserve(clouds.size());
  descriptors.reserve(clouds.size());

  /* compute per-cloud features */

  // resize clouds to registration resolution
  for (auto &cloud : clouds) {
    PointCloudPtr resized = downSample(cloud, params.resolution);
    clouds_resized.emplace_back(std::move(resized));
  }

  // remove noise (this reduces number of keypoints)
  for (auto &cloud : clouds_resized) {
    cloud = removeOutliers(cloud, params.descriptor_radius,
                           params.outliers_min_neighbours);
  }

  // compute normals
  for (const auto &cloud : clouds_resized) {
    auto cloud_normals = computeSurfaceNormals(cloud, params.normal_radius);
    normals.emplace_back(std::move(cloud_normals));
  }

  // detect keypoints
  for (size_t i = 0; i < clouds_resized.size(); ++i) {
    auto cloud_keypoints = detectKeypoints(
        clouds_resized[i], normals[i], params.keypoint_type,
        params.keypoint_threshold, params.normal_radius, params.resolution);
    keypoints.emplace_back(std::move(cloud_keypoints));
  }

  for (size_t i = 0; i < clouds_resized.size(); ++i) {
    auto cloud_descriptors = computeLocalDescriptors(
        clouds_resized[i], normals[i], keypoints[i], params.descriptor_type,
        params.descriptor_radius);
    descriptors.emplace_back(std::move(cloud_descriptors));
  }

  /* estimate pairwise transforms */

  std::vector<TransformEstimate> pairwise_transforms;
  // generate pairs
  for (size_t i = 0; i < clouds.size() - 1; ++i) {
    for (size_t j = i + 1; j < clouds.size(); ++j) {
      if (keypoints[i]->points.size() > 0 && keypoints[j]->points.size() > 0) {
        pairwise_transforms.emplace_back(i, j);
      }
    }
  }

  for (auto &estimate : pairwise_transforms) {
    size_t i = estimate.source_idx;
    size_t j = estimate.target_idx;
    estimate.transform = estimateTransform(
        clouds_resized[i], keypoints[i], descriptors[i], clouds_resized[j],
        keypoints[j], descriptors[j], params.estimation_method,
        params.refine_transform, params.inlier_threshold,
        params.max_correspondence_distance, params.max_iterations,
        params.matching_k, params.transform_epsilon);
    estimate.confidence =
        1. / transformScore(clouds_resized[i], clouds_resized[j],
                            estimate.transform,
                            params.max_correspondence_distance);
  }

  std::vector<Eigen::Matrix4f> global_transforms =
      computeGlobalTransforms(pairwise_transforms, params.confidence_threshold);

  return global_transforms;
}

PointCloudPtr composeMaps(const std::vector<PointCloudPtr> &clouds,
                          const std::vector<Eigen::Matrix4f> &transforms,
                          double resolution)
{
  if (clouds.size() != transforms.size()) {
    throw new std::runtime_error("composeMaps: clouds and transforms size must "
                                 "be the same.");
  }

  PointCloudPtr result(new PointCloud);
  PointCloudPtr cloud_aligned(new PointCloud);
  for (size_t i = 0; i < clouds.size(); ++i) {
    if (transforms[i].isZero()) {
      continue;
    }

    pcl::transformPointCloud(*clouds[i], *cloud_aligned, transforms[i]);
    *result += *cloud_aligned;
  }

  // voxelize result cloud to required resolution
  result = downSample(result, resolution);

  return result;
}
