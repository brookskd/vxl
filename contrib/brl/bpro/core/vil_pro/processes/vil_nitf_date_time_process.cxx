//This is brl/bpro/core/vil_pro/processes/vil_nitf_date_time_process.cxx
//:
// \file

#include <vil/vil_image_resource.h>
#include <vil/vil_load.h>
#include <vil/file_formats/vil_nitf2_image.h>
#include <vcl_cmath.h>
#include <vcl_vector.h>
#include <vcl_iostream.h>
#include <bprb/bprb_func_process.h>
//: set input and output types
bool  vil_nitf_date_time_process_cons(bprb_func_process& pro)
{
  //input, a path to an nitf image 
  vcl_vector<vcl_string> input_types;
  input_types.push_back("vcl_string"); // 0 nitf path
  if (!pro.set_input_types(input_types))
    return false;

  //output, extracts IDATIM property from the NITF image subheader
  vcl_vector<vcl_string> output_types;
  output_types.push_back("int"); // 0 year
  output_types.push_back("int"); // 1 month
  output_types.push_back("int"); // 2 day
  output_types.push_back("int"); // 3 hour
  output_types.push_back("int"); // 4 min
  output_types.push_back("int"); // 5 sec
  return pro.set_output_types(output_types);
}

bool vil_nitf_date_time_process(bprb_func_process& pro)
{

  if (pro.n_inputs()<1)
  {
    vcl_cout << pro.name() << " The input number should be " << 1 << vcl_endl;
    return false;
  }

  //get the inputs
  vcl_string nitf_image_path = pro.get_input<vcl_string>(0);
  //read and parse NITF image
  vil_image_resource_sptr image =
      vil_load_image_resource(nitf_image_path.c_str());
  if (!image)
  {
    vcl_cout << "NITF image load failed in vil_nitf_date_time_process\n";
    return 0;
  }

  vcl_string format = image->file_format();
  vcl_string prefix = format.substr(0,4);

  if (prefix != "nitf")
  {
    vcl_cout << "source image is not NITF in vil_nitf_date_time_process\n";
    return 0;
  }

    //cast to an nitf2_image
  vil_nitf2_image *nitf_image = static_cast<vil_nitf2_image*>(image.ptr());

    //get NITF information
  vcl_vector< vil_nitf2_image_subheader* > headers = nitf_image->get_image_headers();
  // Note, assumes the the file contains exactly one image
  vil_nitf2_image_subheader* hdr = headers[0];
  if(!hdr){
    vcl_cerr << "In vil_nitf_date_time_process: null image subheader\n";
    return false;
  }
  int year, month, day, hour, min, sec;
  bool success = hdr->get_date_time(year, month, day, hour,  min, sec);
  if (!success) {
    vcl_cerr << "In vil_nitf_date_time_process: failed to obtain date time info\n";
    return false;
  }
  //output date time info
  pro.set_output_val<int>(0, year);
  pro.set_output_val<int>(1, month);
  pro.set_output_val<int>(2, day);
  pro.set_output_val<int>(3, hour);
  pro.set_output_val<int>(4, min);
  pro.set_output_val<int>(5, sec);
  return true;
}

