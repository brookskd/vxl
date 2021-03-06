//:
// \file
// \author J.L. Mundy
// \date 11/19/14


#include <testlib/testlib_test.h>
#include <vul/vul_timer.h>
#include <vul/vul_file.h>
#include <vcl_string.h>
#include <vul/vul_timer.h>
#include <vcl_fstream.h>
#include "../boxm2_vecf_point_cloud_orbit_viewer.h"
#include "../boxm2_vecf_orbit_params.h"

#define BUILD_TEST_PC_VIEWER
void test_pc_viewer()
{
#ifdef BUILD_TEST_PC_VIEWER
  boxm2_vecf_point_cloud_orbit_viewer pcv;
  vcl_string base_dir ="c:/Users/mundy/VisionSystems/Janus/experiments/Helena/"; 
  vcl_string pc_in_str = "linden/linden_sampled_points.txt";
  vcl_string pc_in_path = base_dir + pc_in_str;
  vcl_string pc_out_str = "linden/linden_sampled_points_orbit_disp.txt";
  vcl_string pc_out_path = base_dir + pc_out_str;
  vcl_string pc_left_param_str = "linden/linden_left_orbit_params.txt";
  vcl_string pc_left_param_path = base_dir + pc_left_param_str;
   pcv.set_point_cloud(pc_in_path);
  vcl_ifstream istr(pc_left_param_path.c_str());
  if(!istr)
    return;
  boxm2_vecf_orbit_params left_params;
  istr >> left_params;
  pcv.display_orbit(left_params, false);
  pcv.save_point_cloud(pc_out_path);
#endif
}
TESTMAIN( test_pc_viewer );
 
