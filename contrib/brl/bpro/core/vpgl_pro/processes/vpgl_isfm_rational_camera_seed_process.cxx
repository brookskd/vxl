// This is contrib/brl/bpro/core/vpgl_pro/processes/vpgl_isfm_rational_camera_seed_process.cxx
#include <bprb/bprb_func_process.h>
//:
// \file
//      Take a list of rational cameras and a list of 2-d image correspondences
//      Use a RANSAC scheme to find offsets for each camera using "inlier" correspondence
//      Note that the input and output cameras are local rational cameras
//
//      This code was developed under contract #FA8650-13-C-1613 and is approved for public release.
//      The case number for the approval is 88ABW-2014-1143.
//
// \verbatim
//  Modifications
//  Yi Dong, Aug, 2015, remove image dependency
// \endverbatim

#include <bprb/bprb_parameters.h>
#include <vcl_iostream.h>
#include <vcl_sstream.h>
#include <vcl_fstream.h>
#include <vcl_iomanip.h>
#include <vpgl/vpgl_rational_camera.h>
#include <vpgl/vpgl_local_rational_camera.h>
#include <vul/vul_file.h>
#include <vul/vul_awk.h>
#include <vgl/vgl_point_2d.h>
#include <vgl/vgl_point_3d.h>
#include <vpgl/file_formats/vpgl_geo_camera.h>
#include <vpgl/algo/vpgl_rational_adjust_onept.h>
#include <vpgl/algo/vpgl_rational_adjust_multipt.h>
#include <vul/vul_file_iterator.h>
#include <vil/vil_load.h>
#include <vil/vil_image_view.h>
#include <vil/vil_crop.h>
#include <vgl/vgl_distance.h>
#include <vgl/vgl_intersection.h>
#include <brad/brad_image_metadata.h>

//: take a list of rational cameras and a list of 2-d image correspondences of the same 3-d point location
//  find that 3-d location using back-projection and project that point back to images such that each camera can
//  be corrected by adjusting its 2-d image offsets, in a RANSAC scheme
namespace vpgl_isfm_rational_camera_seed_process_globals
{
  unsigned n_inputs_  = 10;
  unsigned n_outputs_ = 0;

  //: find the min and max height in a given region from height map resources
  bool find_min_max_height(double const& ll_lon, double const& ll_lat, double const& uu_lon, double const& uu_lat,
                           vcl_vector<vcl_pair<vil_image_view_base_sptr, vpgl_geo_camera*> >& infos,
                           double& min, double& max);
  void crop_and_find_min_max(vcl_vector<vcl_pair<vil_image_view_base_sptr, vpgl_geo_camera*> >& infos,
                             unsigned const& img_id, int const& i0, int const& j0, int const& crop_ni, int const& crop_nj,
                             double& min, double& max);

  //: return the overlapped region of multiple 2-d bounding box
  vgl_box_2d<double> intersection(vcl_vector<vgl_box_2d<double> > const& boxes);

  //: calculate the relative diameter used in back-projection
  //  Relative diameter is used to define the initial search range in Amoeba algorithm (check vnl/algo/vnl_amoeba.h for more details)
  //bool obtain_relative_diameter(vcl_vector<vcl_string> const& sat_res,
  //                              double& relative_diameter);
  bool obtain_relative_diameter(double const& ll_lon, double const& ll_lat,
                                double const& ur_lon, double const& ur_lat,
                                double& relative_diameter);

  //: calculate the 3-d initial point from the overlapped region of satellite images
  //bool initial_point_by_overlap_region(vcl_vector<vcl_string> const& sat_res,
  //                                     vcl_vector<vcl_pair<vil_image_view_base_sptr, vpgl_geo_camera*> >& dem_infos,
  //                                     vgl_point_3d<double>& init_pt,
  //                                     double& zmin, double& zmax,
  //                                     double const& height_diff = 20.0);
  bool initial_point_by_overlap_region(double const& ll_lon, double const& ll_lat, double const& ur_lon, double const& ur_lat,
                                       vcl_vector<vcl_pair<vil_image_view_base_sptr, vpgl_geo_camera*> >& dem_infos,
                                       vgl_point_3d<double>& init_pt,
                                       double& zmin, double& zmax,
                                       double const& height_diff = 20.0);
}

bool vpgl_isfm_rational_camera_seed_process_cons(bprb_func_process& pro)
{
  using namespace vpgl_isfm_rational_camera_seed_process_globals;

  vcl_vector<vcl_string> input_types(n_inputs_);
  input_types[0] = "vcl_string"; // a file that lists the name to a input camera, file path of the satellite image on each line and i and j coordinate of the 3D world point
                                 // format of the file:
                                 // n  # number of correspondences for each frame,
                                 // full_path_cam_name_1 i_11 j_11 i_12 j_12 ... i_1n j_1n
                                 // full_path_cam_name_2 i_21 j_21 i_22 j_22 ... i_2n j_2n
  input_types[1] = "vcl_string"; // output folder
  input_types[2] = "vcl_string"; // aster dem folder
  input_types[3] = "float";      // lower left longitude
  input_types[4] = "float";      // lower right latitude
  input_types[5] = "float";      // upper left longitude
  input_types[6] = "float";      // upper right latitude
  input_types[7] = "double";     // extra elevation value added onto height search space
  input_types[8] = "float";      // pixel radius for the disagreement among inliers, e.g., 2 pixels
  input_types[9] = "bool";       // option to enforce having at least 2 existing corrected cameras in output folder

  vcl_vector<vcl_string> output_types(n_outputs_);
  return pro.set_input_types(input_types) && pro.set_output_types(output_types);
}

//: execute the process
bool vpgl_isfm_rational_camera_seed_process(bprb_func_process& pro)
{
  using namespace vpgl_isfm_rational_camera_seed_process_globals;
  if (!pro.verify_inputs()) {
    vcl_cerr << pro.name() << ": Wrong Inputs!!!!\n";
    return false;
  }

  // get the inputs
  unsigned in_i = 0;
  vcl_string input_txt  = pro.get_input<vcl_string>(in_i++);
  vcl_string out_folder = pro.get_input<vcl_string>(in_i++);
  vcl_string dem_folder = pro.get_input<vcl_string>(in_i++);
  float lower_left_lon  = pro.get_input<float>(in_i++);
  float lower_left_lat  = pro.get_input<float>(in_i++);
  float upper_right_lon = pro.get_input<float>(in_i++);
  float upper_right_lat = pro.get_input<float>(in_i++);
  double height_diff    = pro.get_input<double>(in_i++);
  float pixel_radius    = pro.get_input<float>(in_i++);
  bool enforce_existing = pro.get_input<bool>(in_i++);

  if (enforce_existing)
    vcl_cout << "!!!!!!! enforce to have at least 2 existing images!\n";
  else
    vcl_cout << "!!!!!!! DO NOT enforce to have at least 2 existing images!\n";
  
  vgl_box_2d<double> overlap_region(lower_left_lon, upper_right_lon, lower_left_lat, upper_right_lat);
  if (overlap_region.is_empty()) {
    vcl_cerr << pro.name() << ": the pre-defined overlap region is empty!!\n";
    return false;
  }
  // read the track file
  vcl_ifstream ifs(input_txt.c_str());
  if (!ifs) {
    vcl_cerr << pro.name() << ": can not open track input file: " << input_txt << "!!\n";
    return false;
  }
  unsigned n;
  ifs >> n;
  if (!n) {
    vcl_cerr << pro.name() << ": 0 correspondences in file: " << input_txt << "! returning without correcting any camera!\n";
    return false;
  }
  vcl_cout << "will read " << n << " correspondences for each frame from " << input_txt << vcl_endl;
  vcl_vector<vcl_string> in_cam_files;
  vcl_vector<vcl_vector<vgl_point_2d<double> > > corrs;
  vcl_vector<vcl_string> out_cam_files;

  while (!ifs.eof())
  {
    vcl_string cam_file;
    ifs >> cam_file;
    if (cam_file.size() < 2)
      break;
    vcl_string out_cam_file = out_folder + vul_file::strip_extension(vul_file::basename(cam_file)) + "_local_corrected.rpb";
    vcl_cout << "reading camera: " << cam_file << vcl_endl;
    vcl_cout << "output camera: " << out_cam_file << vcl_endl;

    vcl_vector<vgl_point_2d<double> > corrs_frame;
    for (unsigned ii = 0; ii < n; ii++) {
      double i, j;
      ifs >> i;  ifs >> j;
      vgl_point_2d<double> cor(i,j);
      corrs_frame.push_back(cor);
    }
    in_cam_files.push_back(cam_file);
    out_cam_files.push_back(out_cam_file);
    corrs.push_back(corrs_frame);
  }
  ifs.close();

  // print out 
  for (unsigned i = 0; i < in_cam_files.size(); i++)
  {
    vcl_cout << "============ " << i << " ============" << vcl_endl;
    vcl_cout << in_cam_files[i] << vcl_endl;
    vcl_cout << out_cam_files[i] << vcl_endl;
    for (unsigned k = 0; k < n; k++) {
      vcl_cout << '[' << corrs[i][k].x() << ',' << corrs[i][k].y() << "] ";
    }
    vcl_cout << '\n' << vcl_flush;
  }

  // load aster dem resources
  vcl_vector<vcl_pair<vil_image_view_base_sptr, vpgl_geo_camera*> > dem_infos;
  vcl_string file_glob = dem_folder + "/*.tif";
  for (vul_file_iterator fn = file_glob.c_str(); fn; ++fn)
  {
    vcl_string filename = fn();
    vil_image_view_base_sptr img_r = vil_load(filename.c_str());
    vpgl_geo_camera* cam;
    vpgl_lvcs_sptr lvcs_dummy = new vpgl_lvcs;
    vil_image_resource_sptr img_res = vil_load_image_resource(filename.c_str());
    if (!vpgl_geo_camera::init_geo_camera(img_res, lvcs_dummy, cam)) {
      vcl_cerr << pro.name() << ": Given height map " << filename << " is NOT a GeoTiff!\n";
      return false;
    }
    dem_infos.push_back(vcl_pair<vil_image_view_base_sptr, vpgl_geo_camera*>(img_r, cam));
  }
  if (dem_infos.empty()) {
    vcl_cerr << pro.name() << ": No image in the folder: " << dem_folder << vcl_endl;
    return false;
  }

  // define camera weight
  vcl_vector<float> cam_weights;
  vcl_vector<vpgl_rational_camera<double> > cams;
  vcl_vector<vpgl_local_rational_camera<double> > local_cams;
  vcl_vector<vcl_vector<vgl_point_2d<double> > > new_corrs;
  unsigned cnt_exist = 0;
  // now determine which cameras already exist
  for (unsigned i = 0; i < out_cam_files.size(); i++)
  {
    if (vul_file::exists(out_cam_files[i]))
    {
      cnt_exist++;
      // load the corrected cameras
      vpgl_local_rational_camera<double> *ratcam = read_local_rational_camera<double>(out_cam_files[i]);
      if (!ratcam) {
        vcl_cerr << pro.name() << ": failed to load rational camera from file " << out_cam_files[i] << "!\n";
        return false;
      }
      cams.push_back(*ratcam);
      local_cams.push_back(*ratcam);
      cam_weights.push_back(1.0f);
      new_corrs.push_back(corrs[i]);
    }
    else // load original camera and start correction
    {
      vpgl_local_rational_camera<double> *ratcam = read_local_rational_camera<double>(in_cam_files[i]);
      if (!ratcam) {
        vcl_cerr << pro.name() << ": failed to load rational camera from file " << in_cam_files[i] << "!\n";
        return false;
      }
      cams.push_back(*ratcam);
      local_cams.push_back(*ratcam);
      cam_weights.push_back(0.0f);
      new_corrs.push_back(corrs[i]);
    }
  }
  if (enforce_existing && cnt_exist < 2) {
    vcl_cerr << pro.name() << ": enforcing condition to have at least 2 pre-existing corrected cameras! EXITING since there is: " << cnt_exist << " cameras.\n";
    return false;
  }
  // re-distribute weight parameters if there is no corrected camera
  if (cnt_exist == 0) {
    cam_weights.assign(cam_weights.size(), 1.0f/cams.size());
  }
  else {
    if (cnt_exist < 2) {
      vcl_cerr << pro.name() << ": If pre-existing camera exists, there should be at least 2 corrected cameras!\n";
      return false;
    }
  }
  vcl_cout << "Assigned camera weights: \n";
  for (unsigned i = 0; i < cams.size(); i++)
    vcl_cout << in_cam_files[i] << " weight: " << cam_weights[i] << vcl_endl;
  vcl_cout << "camera size: " << cams.size() << " corrs size: " << new_corrs.size() << vcl_endl;
  vcl_cout.flush();

  // calculate the initial guessing point
  vgl_point_3d<double> initial_pt;
  double zmin, zmax;
  if (!initial_point_by_overlap_region(lower_left_lon, lower_left_lat, upper_right_lon, upper_right_lat, dem_infos, initial_pt, zmin, zmax, height_diff)) {
    vcl_cerr << pro.name() << ": Evaluating initial point failed!\n";
    return false;
  }

  // calculate the relative diameter given the fact that all correspondence must be inside the overlapped region of all cameras
  double relative_diameter;
  if (!obtain_relative_diameter(lower_left_lon, lower_left_lat, upper_right_lon, upper_right_lat, relative_diameter)) {
    vcl_cerr << pro.name() << ": Evaluating relative diameter failed!\n";
    return false;
  }

  // adjust using each correspondence and save the offsets
  vcl_cout << "Executing adjust image offsets..." << vcl_endl;
  vcl_cout << "initial 3-d point for back projection: " << initial_pt << vcl_endl;
  vcl_cout << "height value (using " << height_diff << " meter height difference): [" << zmin << ',' << zmax << ']' << vcl_endl;
  vcl_cout << "relative diameter: " << relative_diameter << vcl_endl;
  vcl_vector<vcl_vector<vgl_vector_2d<double> > > cam_trans;
  vcl_vector<unsigned> corrs_ids;
  for (unsigned i = 0; i < n; i++)
  {
    // re-arrange the correspondence
    vcl_vector<vgl_point_2d<double> > corrs_i;
    for (unsigned jj = 0; jj < new_corrs.size(); jj++)
      corrs_i.push_back(new_corrs[jj][i]);
    vcl_vector<vgl_vector_2d<double> > cam_trans_i;
    vgl_point_3d<double> intersection;
    if (!vpgl_rational_adjust_onept::adjust_with_weights(cams, cam_weights, corrs_i, initial_pt, zmin, zmax, cam_trans_i, intersection, relative_diameter))
    {
      continue;
#if 1
      vcl_cout << "correspondence adjustment failed for correspondence: " << vcl_endl;
      for (unsigned ii = 0; ii < corrs_i.size(); ii++)
        vcl_cout << "[" << corrs_i[ii].x() << "," << corrs_i[ii].y() << "]\t";
      vcl_cout << '\n';
#endif
    }
    cam_trans.push_back(cam_trans_i);
    corrs_ids.push_back(i);
#if 0
    vcl_cout << i << " --> correspondence: ";
    for (unsigned i = 0; i < corrs_i.size(); i++) {
        vcl_cout << "[" << corrs_i[i].x() << "," << corrs_i[i].y() << "]\t";
    }
    vcl_cout << " --> project to 3D intersection point: [" << vcl_setprecision(12) << intersection.y() 
                                                         << "," << vcl_setprecision(12) << intersection.x() 
                                                         << "," << vcl_setprecision(12) << intersection.z()
                                                         << "], giving offset: ";
    vcl_cout << " --> camera translation: ";
    for (unsigned i = 0; i < cam_trans_i.size(); i++) {
      vcl_cout << "[" << cam_trans_i[i].x() << "," << cam_trans_i[i].y() << "]\t";
    }
    vcl_cout << '\n';
#endif
  }

  vcl_cout << "out of " << n << " correspondences " << cam_trans.size() << " of them back-projected to 3-d world point successfully:";
  for (unsigned i = 0; i < corrs_ids.size(); i++)
    vcl_cout << ' ' << corrs_ids[i];
  vcl_cout << '\n';

  if (!cam_trans.size()) {
    vcl_cout << "out of " << n << " correspondences " << cam_trans.size() << " of them yielded corrections! exit without any correction!\n";
    return false;
  }

  // find the inliers
  vcl_vector<unsigned> inlier_cnts(cam_trans.size(), 0);
  vcl_vector<vcl_vector<unsigned> > inliers;
  for (unsigned i = 0; i < cam_trans.size(); i++) {  // for each correction find how many inliers are there for it
    vcl_vector<unsigned> inliers_i;
    inliers_i.push_back(corrs_ids[i]); // first push itself
    inlier_cnts[i]++;
    for (unsigned j = 0; j < cam_trans.size(); j++) {
      if (i == j) continue;
      double dif = 0;
      for (unsigned k = 0; k < cam_trans[i].size(); k++) {
        vgl_point_2d<double> trans1(cam_trans[i][k].x(), cam_trans[i][k].y());
        vgl_point_2d<double> trans2(cam_trans[j][k].x(), cam_trans[j][k].y());
        dif += vgl_distance(trans1, trans2);
      }
      dif /= cam_trans[i].size();
      if (dif < pixel_radius) {
        inlier_cnts[i]++;
        inliers_i.push_back(corrs_ids[j]);
      }
    }
    inliers.push_back(inliers_i);
  }
  unsigned max = 0;
  unsigned max_i = 0;
  for (unsigned i = 0; i < cam_trans.size(); i++) {
    if (max < inlier_cnts[i]) {
      max = inlier_cnts[i];
      max_i = i;
    }
  }
  vcl_cout << "out of " << cam_trans.size() << " valid correspondences, " << max << " of them yield constant translations using " << pixel_radius << " pixel radius" << vcl_endl;
  // check whether the inliers count is sufficient
  double inlier_ratio = (double)max / cam_trans.size();
  if (inlier_ratio < 0.1) {
    vcl_cout << pro.name() << ": less than 10% of correspondence yield constant translations due to bad correspondence, correction failed" << vcl_endl;
    return false;
  }

#if 1
  vcl_cout << "correspondence that provides inliers: " << vcl_endl;
  for (unsigned j = 0; j < inliers[max_i].size(); j++)
    vcl_cout << inliers[max_i][j] << ' ';
  vcl_cout << '\n';
#endif

  // use the correspondence with the most number of inliers to correct the cameras
  vcl_cout << "correction offset: " << vcl_endl;
  for (unsigned k = 0; k < cams.size(); k++)
    vcl_cout << "camera " << k << " --> offset_u: " << cam_trans[max_i][k].x() << " offset_v: " << cam_trans[max_i][k].y() << vcl_endl;
  for (unsigned k = 0; k < cams.size(); k++) {
    double u_off, v_off;
    cams[k].image_offset(u_off, v_off);
    local_cams[k].set_image_offset(u_off + cam_trans[max_i][k].x(), v_off + cam_trans[max_i][k].y());
    cams[k].set_image_offset(u_off + cam_trans[max_i][k].x(), v_off + cam_trans[max_i][k].y());
  }

  // refine the cameras using all the inliers of this correspondence
  vcl_vector<vcl_vector<vgl_point_2d<double> > > corrs_inliers;
  for (unsigned k = 0; k < new_corrs.size(); k++) {
    vcl_vector<vgl_point_2d<double> > vec;
    for (unsigned j = 0; j < inliers[max_i].size(); j++)
      vec.push_back(new_corrs[k][inliers[max_i][j]]);
    corrs_inliers.push_back(vec);
  }
  vcl_vector<vgl_vector_2d<double> > cam_trans_inliers;
  vcl_vector<vgl_point_3d<double> > intersections;
  if (!vpgl_rational_adjust_multiple_pts::adjust_lev_marq(cams, cam_weights, corrs_inliers, initial_pt, zmin, zmax, cam_trans_inliers, intersections, relative_diameter))
  {
    vcl_cerr << "In vpgl_correct_rational_cameras_process - adjustment failed\n";
    return false;
  }

#if 0
  vcl_cout << " after refinement: \n";
  for (unsigned i = 0; i < intersections.size(); i++)
    vcl_cout << "after adjustment 3D intersection point: " << intersections[i].y() << "," << intersections[i].x() 
                                                           << "," << intersections[i].z()
                                                           << vcl_endl;
#endif

  // further correction using refined offset values
  for (unsigned i = 0; i < cams.size(); i++)
  {
    double u_off, v_off;
    cams[i].image_offset(u_off, v_off);
    local_cams[i].set_image_offset(u_off+cam_trans_inliers[i].x(), v_off+cam_trans_inliers[i].y());
    local_cams[i].save(out_cam_files[i]);
  }
  return true;
}

#if 0
bool vpgl_isfm_rational_camera_seed_process_globals::initial_point_by_overlap_region(vcl_vector<vcl_string> const& sat_res,
                                                                                     vcl_vector<vcl_pair<vil_image_view_base_sptr, vpgl_geo_camera*> >& dem_infos,
                                                                                     vgl_point_3d<double>& init_pt,
                                                                                     double& zmin, double& zmax, double const& height_diff)
{
  // obtain the overlapped region
  vcl_vector<vgl_box_2d<double> > img_footprints;
  for (vcl_vector<vcl_string>::const_iterator vit = sat_res.begin(); vit != sat_res.end(); ++vit)
  {
    brad_image_metadata meta(*vit);
    double ll_lon = meta.lower_left_.x();
    double ll_lat = meta.lower_left_.y();
    double ur_lon = meta.upper_right_.x();
    double ur_lat = meta.upper_right_.y();
    vgl_box_2d<double> img_box(ll_lon, ur_lon, ll_lat, ur_lat);
    img_footprints.push_back(img_box);
  }
  vgl_box_2d<double> overlap_region = vpgl_isfm_rational_camera_seed_process_globals::intersection(img_footprints);
  if (overlap_region.is_empty())
    return false;
  // find the min and max elevation value of the overlap region
  double min_elev = 10000.0, max_elev = -10000.0;
  if (!vpgl_isfm_rational_camera_seed_process_globals::find_min_max_height(overlap_region.min_x(), overlap_region.min_y(), overlap_region.max_x(), overlap_region.max_y(),
                                                                           dem_infos, min_elev, max_elev))
    return false;
  zmin = min_elev - 10 - height_diff;
  zmax = max_elev + 10 + height_diff;
  init_pt.set(overlap_region.centroid_x(), overlap_region.centroid_y(), zmin);
  return true;
}
#endif

bool vpgl_isfm_rational_camera_seed_process_globals::initial_point_by_overlap_region(double const& ll_lon, double const& ll_lat,
                                                                                     double const& ur_lon, double const& ur_lat,
                                                                                     vcl_vector<vcl_pair<vil_image_view_base_sptr, vpgl_geo_camera*> >& dem_infos,
                                                                                     vgl_point_3d<double>& init_pt,
                                                                                     double& zmin, double& zmax, double const& height_diff)
{
  vgl_box_2d<double> overlap_region(ll_lon, ur_lon, ll_lat, ur_lat);
  if (overlap_region.is_empty())
    return false;
  // find the min and max elevation value of the overlap region
  double min_elev = 10000.0, max_elev = -10000.0;
  if (!vpgl_isfm_rational_camera_seed_process_globals::find_min_max_height(overlap_region.min_x(), overlap_region.min_y(), overlap_region.max_x(), overlap_region.max_y(),
                                                                          dem_infos, min_elev, max_elev))
    return false;
  zmin = min_elev - 10 - height_diff;
  zmax = max_elev + 10 + height_diff;
  init_pt.set(overlap_region.centroid_x(), overlap_region.centroid_y(), zmin);
  return true;
}

#if 0
bool vpgl_isfm_rational_camera_seed_process_globals::obtain_relative_diameter(vcl_vector<vcl_string> const& sat_res,
                                                                              double& relative_diameter)
{
  relative_diameter = 1.0;
  // obtain the overlap region
  vcl_vector<vgl_box_2d<double> > img_footprints;
  for (vcl_vector<vcl_string>::const_iterator vit = sat_res.begin(); vit != sat_res.end(); ++vit)
  {
    brad_image_metadata meta(*vit);
    double ll_lon = meta.lower_left_.x();
    double ll_lat = meta.lower_left_.y();
    double ur_lon = meta.upper_right_.x();
    double ur_lat = meta.upper_right_.y();
    vgl_box_2d<double> img_box(ll_lon, ur_lon, ll_lat, ur_lat);
    img_footprints.push_back(img_box);
  }
  vgl_box_2d<double> overlap_region = vpgl_isfm_rational_camera_seed_process_globals::intersection(img_footprints);
  if (overlap_region.is_empty())
    return false;
  double width  = overlap_region.width();
  double height = overlap_region.height();
  // calculate the diameter by the diagonal
  double diagonal = vcl_sqrt(width*width + height*height);
  if (overlap_region.centroid_x() > overlap_region.centroid_y())
    relative_diameter = 0.5*diagonal/overlap_region.centroid_y();
  else
    relative_diameter = 0.5*diagonal/overlap_region.centroid_x();

  return true;
}
#endif

bool vpgl_isfm_rational_camera_seed_process_globals::obtain_relative_diameter(double const& ll_lon, double const& ll_lat,
                                                                              double const& ur_lon, double const& ur_lat,
                                                                              double& relative_diameter)
{
  vgl_box_2d<double> overlap_region(ll_lon, ur_lon, ll_lat, ur_lat);
  if (overlap_region.is_empty())
    return false;
  double width  = overlap_region.width();
  double height = overlap_region.height();
  // calculate the diameter by the diagonal
  double diagonal = vcl_sqrt(width*width + height*height);
  if (overlap_region.centroid_x() > overlap_region.centroid_y())
    relative_diameter = 0.5*diagonal/overlap_region.centroid_y();
  else
    relative_diameter = 0.5*diagonal/overlap_region.centroid_x();
  return true;
}

vgl_box_2d<double> vpgl_isfm_rational_camera_seed_process_globals::intersection(vcl_vector<vgl_box_2d<double> > const& boxes)
{
  if (boxes.size() == 2) {
    return vgl_intersection(boxes[0], boxes[1]);
  }
  vcl_vector<vgl_box_2d<double> > new_boxes;
  vgl_box_2d<double> box = vgl_intersection(boxes[0], boxes[1]);
  if (box.is_empty())
    return box;
  new_boxes.push_back(box);
  for (unsigned i = 2; i < boxes.size(); i++)
    new_boxes.push_back(boxes[i]);
  return vpgl_isfm_rational_camera_seed_process_globals::intersection(new_boxes);
}

bool vpgl_isfm_rational_camera_seed_process_globals::find_min_max_height(double const& ll_lon, double const& ll_lat, double const& ur_lon, double const& ur_lat,
                                                                         vcl_vector<vcl_pair<vil_image_view_base_sptr, vpgl_geo_camera*> >& infos,
                                                                         double& min, double& max)
{
  // find the corner points
  vcl_vector<vcl_pair<unsigned, vcl_pair<int, int> > > corners;
  vcl_vector<vgl_point_2d<double> > pts;
  pts.push_back(vgl_point_2d<double>(ll_lon, ur_lat));
  pts.push_back(vgl_point_2d<double>(ur_lon, ll_lat));
  pts.push_back(vgl_point_2d<double>(ll_lon, ll_lat));
  pts.push_back(vgl_point_2d<double>(ur_lon, ur_lat));
  for (unsigned k = 0; k < (unsigned)pts.size(); k++)
  {
    // find the image
    for (unsigned j = 0; j < (unsigned)infos.size(); j++)
    {
      double u, v;
      infos[j].second->global_to_img(pts[k].x(), pts[k].y(), 0, u, v);
      int uu = (int)vcl_floor(u+0.5);
      int vv = (int)vcl_floor(v+0.5);
      if (uu < 0 || vv < 0 || uu >= (int)infos[j].first->ni() || vv >= (int)infos[j].first->nj())
        continue;
      vcl_pair<unsigned, vcl_pair<int, int> > pp(j, vcl_pair<int, int>(uu, vv));
      corners.push_back(pp);
      break;
    }
  }
  if (corners.size() != 4) {
    vcl_cerr << "Cannot locate all 4 corners among given DEM tiles!\n";
    return false;
  }
  // case 1 all corners are in the same image
  if (corners[0].first == corners[1].first) {
    // crop the image
    int i0 = corners[0].second.first;
    int j0 = corners[0].second.second;
    int crop_ni = corners[1].second.first-corners[0].second.first+1;
    int crop_nj = corners[1].second.second-corners[0].second.second+1;
    vpgl_isfm_rational_camera_seed_process_globals::crop_and_find_min_max(infos, corners[0].first, i0, j0, crop_ni, crop_nj, min, max);
    return true;
  }
  // case 2: two corners are in the same image
  if (corners[0].first == corners[2].first && corners[1].first == corners[3].first) {
    // crop the first image
    int i0 = corners[0].second.first;
    int j0 = corners[0].second.second;
    int crop_ni = infos[corners[0].first].first->ni() - corners[0].second.first;
    int crop_nj = corners[2].second.second-corners[0].second.second+1;
    vpgl_isfm_rational_camera_seed_process_globals::crop_and_find_min_max(infos, corners[0].first, i0, j0, crop_ni, crop_nj, min, max);
    
    // crop the second image
    i0 = 0;
    j0 = corners[3].second.second;
    crop_ni = corners[3].second.first + 1;
    crop_nj = corners[1].second.second-corners[3].second.second+1;
    vpgl_isfm_rational_camera_seed_process_globals::crop_and_find_min_max(infos, corners[1].first, i0, j0, crop_ni, crop_nj, min, max);
    return true;
  }
  // case 3: two corners are in the same image
  if (corners[0].first == corners[3].first && corners[1].first == corners[2].first) {
    // crop the first image
    int i0 = corners[0].second.first;
    int j0 = corners[0].second.second;
    int crop_ni = corners[3].second.first - corners[0].second.first + 1;
    int crop_nj = infos[corners[0].first].first->nj() - corners[0].second.second; 
    vpgl_isfm_rational_camera_seed_process_globals::crop_and_find_min_max(infos, corners[0].first, i0, j0, crop_ni, crop_nj, min, max);
    
    // crop the second image
    i0 = corners[2].second.first;
    j0 = 0; 
    crop_ni = corners[1].second.first - corners[2].second.first + 1;
    crop_nj = corners[2].second.second + 1;
    vpgl_isfm_rational_camera_seed_process_globals::crop_and_find_min_max(infos, corners[1].first, i0, j0, crop_ni, crop_nj, min, max);
    return true;
  }
  // case 4: all corners are in a different image
  // crop the first image, image of corner 0
  int i0 = corners[0].second.first;
  int j0 = corners[0].second.second;
  int crop_ni = infos[corners[0].first].first->ni() - corners[0].second.first;
  int crop_nj = infos[corners[0].first].first->nj() - corners[0].second.second;
  vpgl_isfm_rational_camera_seed_process_globals::crop_and_find_min_max(infos, corners[0].first, i0, j0, crop_ni, crop_nj, min, max);
  
  // crop the second image, image of corner 1
  i0 = 0;
  j0 = 0;
  crop_ni = corners[1].second.first + 1;
  crop_nj = corners[1].second.second + 1;
  vpgl_isfm_rational_camera_seed_process_globals::crop_and_find_min_max(infos, corners[1].first, i0, j0, crop_ni, crop_nj, min, max);
  
  // crop the third image, image of corner 2
  i0 = corners[2].second.first;
  j0 = 0;
  crop_ni = infos[corners[2].first].first->ni() - corners[2].second.first;
  crop_nj = corners[2].second.second + 1;
  vpgl_isfm_rational_camera_seed_process_globals::crop_and_find_min_max(infos, corners[2].first, i0, j0, crop_ni, crop_nj, min, max);
  
  // crop the fourth image, image of corner 3
  i0 = 0;
  j0 = corners[3].second.second;
  crop_ni = corners[3].second.first + 1;
  crop_nj = infos[corners[3].first].first->nj() - corners[3].second.second;
  vpgl_isfm_rational_camera_seed_process_globals::crop_and_find_min_max(infos, corners[3].first, i0, j0, crop_ni, crop_nj, min, max);
  return true;
}

void vpgl_isfm_rational_camera_seed_process_globals::crop_and_find_min_max(vcl_vector<vcl_pair<vil_image_view_base_sptr, vpgl_geo_camera*> >& infos,
                                                                           unsigned const& img_id, int const& i0, int const& j0, int const& crop_ni, int const& crop_nj,
                                                                           double& min, double& max)
{
  if (vil_image_view<vxl_int_16>* img = dynamic_cast<vil_image_view<vxl_int_16>*>(infos[img_id].first.ptr()))
  {
    vil_image_view<vxl_int_16> img_crop = vil_crop(*img, i0, crop_ni, j0, crop_nj);
    for (unsigned ii = 0; ii < img_crop.ni(); ii++) {
      for (unsigned jj = 0; jj < img_crop.nj(); jj++) {
        if (min > img_crop(ii, jj)) min = img_crop(ii,jj);
        if (max < img_crop(ii, jj)) max = img_crop(ii,jj);
      }
    }
  }
  else if (vil_image_view<float>* img = dynamic_cast<vil_image_view<float>*>(infos[img_id].first.ptr()))
  {
    vil_image_view<float> img_crop = vil_crop(*img, i0, crop_ni, j0, crop_nj);
    for (unsigned ii = 0; ii < img_crop.ni(); ii++) {
      for (unsigned jj = 0; jj < img_crop.nj(); jj++) {
        if (min > img_crop(ii, jj)) min = img_crop(ii,jj);
        if (max < img_crop(ii, jj)) max = img_crop(ii,jj);
      }
    }
  }
  return;
}