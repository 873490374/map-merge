#include <map_merge_3d/typedefs.h>

#include <map_merge_3d/features.h>
#include <map_merge_3d/matching.h>
#include <map_merge_3d/visualise.h>

#include <pcl/common/time.h>
#include <pcl/common/transforms.h>
#include <pcl/console/parse.h>
#include <pcl/io/pcd_io.h>

void printPointCloud2Summary(const pcl::PCLPointCloud2 &v)
{
  std::cout << "header: " << std::endl;
  std::cout << v.header;
  std::cout << "height: ";
  std::cout << "  " << v.height << std::endl;
  std::cout << "width: ";
  std::cout << "  " << v.width << std::endl;
  std::cout << "fields[]" << std::endl;
  for (size_t i = 0; i < v.fields.size(); ++i) {
    std::cout << "  fields[" << i << "]: ";
    std::cout << std::endl;
    std::cout << "    " << v.fields[i] << std::endl;
  }
  std::cout << "is_bigendian: ";
  std::cout << "  " << v.is_bigendian << std::endl;
  std::cout << "point_step: ";
  std::cout << "  " << v.point_step << std::endl;
  std::cout << "row_step: ";
  std::cout << "  " << v.row_step << std::endl;
  std::cout << "is_dense: ";
  std::cout << "  " << v.is_dense << std::endl;
}

int main(int argc, char **argv)
{
  double resolution = 0.1;
  int min_neighbours = 50;
  const int nr_octaves = 3;
  const int nr_octaves_per_scale = 3;
  const double min_contrast = 5.0;
  double normal_radius = resolution * 6.;
  double descriptor_radius = resolution * 8.;
  double filtering_radius = resolution * 2.;
  const double inlier_threshold = resolution * 5.;
  const double max_correspondence_distance = inlier_threshold * 2.;
  const int nr_iterations = 1000;
  const int max_iterations = 200;
  std::string descriptor_name = "PFH";

  pcl::console::parse_argument(argc, argv, "--normal_radius", normal_radius);
  pcl::console::parse_argument(argc, argv, "--descriptor_radius",
                               descriptor_radius);
  pcl::console::parse_argument(argc, argv, "--resolution", resolution);
  pcl::console::parse_argument(argc, argv, "--min_neighbours", min_neighbours);
  pcl::console::parse_argument(argc, argv, "--filtering_radius",
                               filtering_radius);
  pcl::console::parse_argument(argc, argv, "--descriptor", descriptor_name);

  PointCloudPtr cloud1(new PointCloud);
  PointCloudPtr cloud2(new PointCloud);

  // Load object and scene
  if (pcl::io::loadPCDFile<PointT>(argv[1], *cloud1) < 0 ||
      pcl::io::loadPCDFile<PointT>(argv[2], *cloud2) < 0) {
    pcl::console::print_error("Error loading object/scene file!\n");
    return (1);
  }
  pcl::console::print_highlight("Pointclouds loaded.\n");

  cloud1 = downSample(cloud1, resolution);
  cloud2 = downSample(cloud2, resolution);

  visualisePointCloud(cloud1);

  cloud1 = removeOutliers(cloud1, descriptor_radius, min_neighbours);
  cloud2 = removeOutliers(cloud2, descriptor_radius, min_neighbours);

  visualisePointCloud(cloud1);

  /* detect keypoints */
  PointCloudPtr keypoints1, keypoints2;
  {
    pcl::ScopeTime t("feature detection");
    keypoints1 = detectKeypoints(cloud1, resolution, nr_octaves,
                                 nr_octaves_per_scale, min_contrast);
    keypoints2 = detectKeypoints(cloud2, resolution, nr_octaves,
                                 nr_octaves_per_scale, min_contrast);
  }

  visualiseKeypoints(cloud1, keypoints1);

  /* compute descriptors */
  LocalDescriptorsPtr descriptors1, descriptors2;
  SurfaceNormalsPtr normals1, normals2;
  Descriptor descriptor_type = fromString(descriptor_name);
  {
    pcl::ScopeTime t("descriptors computation");
    normals1 = computeSurfaceNormals(cloud1, normal_radius);
    descriptors1 = computeLocalDescriptors(cloud1, normals1, keypoints1,
                                           descriptor_type, descriptor_radius);
    normals2 = computeSurfaceNormals(cloud2, normal_radius);
    descriptors2 = computeLocalDescriptors(cloud2, normals2, keypoints2,
                                           descriptor_type, descriptor_radius);
  }

  std::cout << "extracted descriptors:" << std::endl;
  printPointCloud2Summary(*descriptors1);

  visualiseNormals(cloud1, normals1);

  /* compute correspondences */
  CorrespondencesPtr inliers;
  Eigen::Matrix4f transform;

  {
    pcl::ScopeTime t("finding correspondences");
    CorrespondencesPtr correspondences =
        findFeatureCorrespondences(descriptors1, descriptors2);
    transform = estimateTransformFromCorrespondences(
        keypoints1, keypoints2, correspondences, inliers, inlier_threshold);
  }

  visualiseCorrespondences(cloud1, keypoints1, cloud2, keypoints2, inliers);

  // Transform the source point to align them with the target points
  PointCloudPtr cloud1_aligned(new PointCloud);
  pcl::transformPointCloud(*cloud1, *cloud1_aligned, transform);
  visualisePointClouds(cloud1_aligned, cloud2);

  {
    pcl::ScopeTime t("initial alignment");
    transform = estimateTransformFromDescriptorsSets(
        keypoints1, descriptors1, keypoints2, descriptors2, inlier_threshold,
        max_correspondence_distance, nr_iterations);
  }

  // Transform the source point to align them with the target points
  pcl::transformPointCloud(*cloud1, *cloud1_aligned, transform);
  visualisePointClouds(cloud1_aligned, cloud2);

  PointCloudPtr aligned(new PointCloud);
  {
    pcl::ScopeTime t("ICP alignment");
    transform = estimateTransformICP(cloud1, cloud2, transform,
                                     max_correspondence_distance,
                                     inlier_threshold, max_iterations, 1e-3);
  }
  pcl::transformPointCloud(*cloud1, *cloud1_aligned, transform);
  visualisePointClouds(cloud1_aligned, cloud2);

  return 0;
}
