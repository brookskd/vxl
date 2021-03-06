// This is brl/bseg/bstm/ocl/algo/bstm_ocl_update_viewdep.cxx
#include "bstm_ocl_update_viewdep.h"
//:
// \file
// \brief  A process for updating a model
//
// \author Ali Osman Ulusoy
// \date May 10, 2013

#include <vcl_fstream.h>
#include <vcl_algorithm.h>
#include <bstm/ocl/bstm_opencl_cache.h>
#include <bstm/bstm_scene.h>
#include <bstm/bstm_block.h>
#include <bstm/bstm_data_base.h>
#include <bstm/ocl/bstm_ocl_util.h>
#include <bstm/bstm_util.h>
#include <boxm2/ocl/algo/boxm2_ocl_camera_converter.h>
#include <vil/vil_image_view.h>

//directory utility
#include <vul/vul_timer.h>
#include <vcl_where_root_dir.h>
#include <bocl/bocl_device.h>
#include <bocl/bocl_kernel.h>

#include <vil/vil_save.h>
//: Map of kernels should persist between process executions
vcl_map<vcl_string,vcl_vector<bocl_kernel*> > bstm_ocl_update_viewdep::kernels_;

//Main public method, updates color model
bool bstm_ocl_update_viewdep::update(bstm_scene_sptr         scene,
                                bocl_device_sptr         device,
                                bstm_opencl_cache_sptr  opencl_cache,
                                vpgl_camera_double_sptr  cam,
                                vil_image_view_base_sptr img,
                                float                   time,
                                float                   mog_var,
                                vil_image_view_base_sptr mask_sptr,
                                bool                     update_alpha,
                                bool                    update_changes_only)
{

  enum {
     UPDATE_SEGLEN = 0,
     UPDATE_PREINF = 1,
     UPDATE_PROC   = 2,
     UPDATE_BAYES  = 3,
     UPDATE_CELL   = 4
   };
   float transfer_time=0.0f;
   float gpu_time=0.0f;
   vcl_size_t local_threads[2]={8,8};
   vcl_size_t global_threads[2]={8,8};

   //catch a "null" mask (not really null because that throws an error)
   bool use_mask = false;
#if 1
   if ( mask_sptr->ni() == img->ni() && mask_sptr->nj() == img->nj() ) {
     vcl_cout << "Update using mask..." << vcl_endl;
     use_mask = true;
   }
#endif
   vil_image_view<unsigned char >* mask_map = 0;
   if (use_mask) {
     mask_map = dynamic_cast<vil_image_view<unsigned char> *>(mask_sptr.ptr());
     if (!mask_map) {
       vcl_cout<<"bstm_update_process:: mask map is not an unsigned char map"<<vcl_endl;
       return false;
     }
   }

   //cache size sanity check
   vcl_size_t binCache = opencl_cache.ptr()->bytes_in_cache();
   vcl_cout<<"Update MBs in cache: "<<binCache/(1024.0*1024.0)<<vcl_endl;

   //make correct data types are here
   vcl_string data_type, num_obs_type,options = "";
   int appTypeSize;
   if (!validate_appearances(scene, data_type, appTypeSize, num_obs_type, options))
     return false;

   // create a command queue.
   int status=0;
   cl_command_queue queue = clCreateCommandQueue( device->context(), *(device->device_id()), CL_QUEUE_PROFILING_ENABLE, &status);
   if (status!=0)
     return false;

   // compile the kernel if not already compiled
   vcl_vector<bocl_kernel*>& kernels = get_kernels(device, options);

   //grab input image, establish cl_ni, cl_nj (so global size is divisible by local size)
   vil_image_view_base_sptr float_img = bstm_util::prepare_input_image(img, true);
   vil_image_view<float>* img_view = static_cast<vil_image_view<float>* >(float_img.ptr());
   unsigned cl_ni=(unsigned)RoundUp(img_view->ni(),(int)local_threads[0]);
   unsigned cl_nj=(unsigned)RoundUp(img_view->nj(),(int)local_threads[1]);
   global_threads[0]=cl_ni;
   global_threads[1]=cl_nj;

   //set generic cam
   cl_float* ray_origins    = new cl_float[4*cl_ni*cl_nj];
   cl_float* ray_directions = new cl_float[4*cl_ni*cl_nj];
   bocl_mem_sptr ray_o_buff = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(cl_float4), ray_origins, "ray_origins buffer");
   bocl_mem_sptr ray_d_buff = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(cl_float4), ray_directions, "ray_directions buffer");
   boxm2_ocl_camera_converter::compute_ray_image( device, queue, cam, cl_ni, cl_nj, ray_o_buff, ray_d_buff);

   //Visibility, Preinf, Norm, and input image buffers
   float* vis_buff = new float[cl_ni*cl_nj];
   float* pre_buff = new float[cl_ni*cl_nj];
   float* norm_buff = new float[cl_ni*cl_nj];
   float* input_buff=new float[cl_ni*cl_nj];
   for (unsigned i=0;i<cl_ni*cl_nj;i++)
   {
     vis_buff[i]=1.0f;
     pre_buff[i]=0.0f;
     norm_buff[i]=0.0f;
   }

   //copy input vals into image
   int count=0;
   for (unsigned int j=0;j<cl_nj;++j) {
     for (unsigned int i=0;i<cl_ni;++i) {
       input_buff[count] = 0.0f;
       if ( i<img_view->ni() && j< img_view->nj() )
         input_buff[count] = (*img_view)(i,j);
       ++count;
     }
   }
   if (use_mask)
   {
     int count = 0;
     for (unsigned int j=0;j<cl_nj;++j) {
       for (unsigned int i=0;i<cl_ni;++i) {
         if ( i<mask_map->ni() && j<mask_map->nj() )
           if ( (*mask_map)(i,j)==0 ) {
             input_buff[count] = -1.0f;
             vis_buff  [count] = -1.0f;
           }
         ++count;
       }
     }
   }

   bocl_mem_sptr in_image = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float), input_buff, "input image buffer");
   in_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

   bocl_mem_sptr vis_image = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float), vis_buff, "vis image buffer");
   vis_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

   bocl_mem_sptr pre_image = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float), pre_buff, "pre image buffer");
   pre_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

   bocl_mem_sptr norm_image = opencl_cache->alloc_mem(cl_ni*cl_nj*sizeof(float), norm_buff, "norm image buffer");
   norm_image->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);


   // Image Dimensions
   int img_dim_buff[4];
   img_dim_buff[0] = 0;
   img_dim_buff[1] = 0;
   img_dim_buff[2] = img_view->ni();
   img_dim_buff[3] = img_view->nj();

   bocl_mem_sptr img_dim=new bocl_mem(device->context(), img_dim_buff, sizeof(int)*4, "image dims");
   img_dim->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

   // Output Array
   float output_arr[100];
   for (int i=0; i<100; ++i) output_arr[i] = -1.0f;
   bocl_mem_sptr  cl_output=new bocl_mem(device->context(), output_arr, sizeof(float)*100, "output buffer");
   cl_output->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

   // bit lookup buffer
   cl_uchar lookup_arr[256];
   bstm_ocl_util::set_bit_lookup(lookup_arr);
   bocl_mem_sptr lookup=new bocl_mem(device->context(), lookup_arr, sizeof(cl_uchar)*256, "bit lookup buffer");
   lookup->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

   // app density used for proc_norm_image
   //Set app_buffer.x = 1.0, if you want uniform background model
   //Set app_buffer.x = 0, app_buffer.y and app_buffer.z to the mean and sigma of the gaussian for the background model
   float app_buffer[4]={1.0,0.0,0.00,0.0};
   bocl_mem_sptr app_density = new bocl_mem(device->context(), app_buffer, sizeof(cl_float4), "app density buffer");
   app_density->create_buffer(CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR);

   //mog variance, if 0.0f or less, then var of app models will be estimated, otherwise, it will be fixed
   bocl_mem_sptr mog_var_mem = new bocl_mem(device->context(), &mog_var, sizeof(mog_var), "update gauss variance");
   mog_var_mem->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

   // update_alpha boolean buffer
   cl_int up_alpha[1];
   up_alpha[0] = update_alpha ? 1 : 0;
   bocl_mem_sptr up_alpha_mem = new bocl_mem(device->context(), up_alpha, sizeof(up_alpha), "update alpha bool buffer");
   up_alpha_mem->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

   cl_int use_mask_buf[1];
   use_mask_buf[0] = use_mask ? 1 : 0;
   bocl_mem_sptr use_mask_mem = new bocl_mem(device->context(), use_mask_buf, sizeof(use_mask_buf), "update with mask");
   use_mask_mem->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

   cl_float cl_time = 0;
   bocl_mem_sptr time_mem =new bocl_mem(device->context(), &cl_time, sizeof(cl_float), "time instance buffer");
   time_mem->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

   // update_alpha boolean buffer
   cl_int up_changes_only[1];
   up_changes_only[0] = update_changes_only ? 1 : 0;
   bocl_mem_sptr up_changes_mem = new bocl_mem(device->context(), up_changes_only, sizeof(up_changes_only), "update changes bool buffer");
   up_changes_mem->create_buffer(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR);

   // set arguments
   vcl_vector<bstm_block_id> vis_order = scene->get_vis_blocks(cam);
   vcl_vector<bstm_block_id>::iterator id;
   for (unsigned int i=0; i<kernels.size(); ++i)
   {
     if ( i == UPDATE_PROC ) {
       bocl_kernel * proc_kern=kernels[i];

       proc_kern->set_arg( norm_image.ptr() );
       proc_kern->set_arg( vis_image.ptr() );
       proc_kern->set_arg( pre_image.ptr());
       proc_kern->set_arg( img_dim.ptr() );
       proc_kern->set_arg( in_image.ptr() );
       proc_kern->set_arg( app_density.ptr() );

       //execute kernel
       proc_kern->execute( queue, 2, local_threads, global_threads);
       int status = clFinish(queue);
       if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
         return false;
       proc_kern->clear_args();
       proc_kern->release_current_event();
       continue;
     }

     for (id = vis_order.begin(); id != vis_order.end(); ++id)
     {
       //choose correct render kernel
       bocl_kernel* kern = kernels[i];

       //if the current blk does not contain the queried time, no need to ray cast
       bstm_block_metadata mdata = scene->get_block_metadata(*id);
       double local_time;
       if(!mdata.contains_t(time,local_time))
         continue;
       //write cl_time
       cl_time = (cl_float)local_time;
       time_mem->write_to_buffer(queue);


       //grab data from cache
       vul_timer transfer;
       bocl_mem* blk       = opencl_cache->get_block(*id);
       bocl_mem* blk_t     = opencl_cache->get_time_block(*id);
       bocl_mem* alpha     = opencl_cache->get_data<BSTM_ALPHA>(*id,0,false);
       bocl_mem* blk_info  = opencl_cache->loaded_block_info();
       bocl_mem* blk_t_info= opencl_cache->loaded_time_block_info();
       bstm_scene_info* info_buffer_t = (bstm_scene_info*) blk_t_info->cpu_buffer();

       //figure out sizes
       int alphaTypeSize = (int)bstm_data_info::datasize(bstm_data_traits<BSTM_ALPHA>::prefix());
       int data_buffer_length = alpha->num_bytes() / alphaTypeSize;
       int num_time_trees = info_buffer_t->tree_buffer_length;
       info_buffer_t->data_buffer_length = data_buffer_length;
       blk_t_info->write_to_buffer((queue));

       //grab MOG
       int nobsTypeSize = (int)bstm_data_info::datasize(num_obs_type);
       bocl_mem* mog       = opencl_cache->get_data(*id,data_type,data_buffer_length*appTypeSize,false);

       //grab AUX
       int auxTypeSize = (int)bstm_data_info::datasize(bstm_data_traits<BSTM_AUX0>::prefix());
       bocl_mem *aux0   = opencl_cache->get_data<BSTM_AUX0>(*id, num_time_trees*auxTypeSize,false);
       bocl_mem *aux1   = opencl_cache->get_data<BSTM_AUX1>(*id, num_time_trees*auxTypeSize,false);
       bocl_mem *aux2   = opencl_cache->get_data<BSTM_AUX2>(*id, num_time_trees*auxTypeSize,false);
       bocl_mem *aux3   = opencl_cache->get_data<BSTM_AUX3>(*id, num_time_trees*auxTypeSize,false);
       auxTypeSize = (int)bstm_data_info::datasize(bstm_data_traits<BSTM_RAY_DIR>::prefix());
       bocl_mem *raydir   = opencl_cache->get_data<BSTM_RAY_DIR>(*id, num_time_trees*auxTypeSize,false);

       transfer_time += (float) transfer.all();
       if (i==UPDATE_SEGLEN)
       {
         aux0->zero_gpu_buffer(queue);
         aux1->zero_gpu_buffer(queue);

         kern->set_arg( blk_info );
         kern->set_arg( blk );
         kern->set_arg( blk_t );
         kern->set_arg( alpha );
         kern->set_arg( aux0 );
         kern->set_arg( aux1 );
         kern->set_arg( lookup.ptr() );
         kern->set_arg( ray_o_buff.ptr() );
         kern->set_arg( ray_d_buff.ptr() );
         kern->set_arg( img_dim.ptr() );
         kern->set_arg( in_image.ptr() );
         kern->set_arg( time_mem.ptr() );
         kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar16) );//local tree,
         kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar8) ); //local time tree
         kern->set_local_arg( local_threads[0]*local_threads[1]*10*sizeof(cl_uchar) ); //cumsum buffer, imindex buffer

         //execute kernel
         kern->execute(queue, 2, local_threads, global_threads);
         int status = clFinish(queue);
         if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
           return false;
         gpu_time += kern->exec_time();

         //clear render kernel args so it can reset em on next execution
         kern->clear_args();
         kern->release_current_event();
       }
       else if (i==UPDATE_PREINF)
       {
         raydir->zero_gpu_buffer(queue);

         kern->set_arg( blk_info );
         kern->set_arg( blk );
         kern->set_arg( blk_t );
         kern->set_arg( alpha );
         kern->set_arg( mog );
         kern->set_arg( raydir );
         kern->set_arg( aux0 );
         kern->set_arg( aux1 );
         kern->set_arg( lookup.ptr() );
         kern->set_arg( ray_o_buff.ptr() );
         kern->set_arg( ray_d_buff.ptr() );
         kern->set_arg( img_dim.ptr() );
         kern->set_arg( vis_image.ptr() );
         kern->set_arg( pre_image.ptr() );
         kern->set_arg( time_mem.ptr() );
         kern->set_arg( cl_output.ptr() );
         kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar16) );//local tree,
         kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar8) ); //local time tree
         kern->set_local_arg( local_threads[0]*local_threads[1]*10*sizeof(cl_uchar) ); //cumsum buffer, imindex buffer
         //execute kernel
         kern->execute(queue, 2, local_threads, global_threads);
         int status = clFinish(queue);
         if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
           return false;
         gpu_time += kern->exec_time();

         //clear render kernel args so it can reset em on next execution
         kern->clear_args();
         kern->release_current_event();
       }
       else if (i==UPDATE_BAYES)
       {
         aux2->zero_gpu_buffer(queue);
         aux3->zero_gpu_buffer(queue);

         kern->set_arg( blk_info );
         kern->set_arg( blk );
         kern->set_arg( blk_t );
         kern->set_arg( alpha );
         kern->set_arg( mog );
         kern->set_arg( aux0 );
         kern->set_arg( aux1 );
         kern->set_arg( aux2 );
         kern->set_arg( aux3 );
         kern->set_arg( lookup.ptr() );
         kern->set_arg( ray_o_buff.ptr() );
         kern->set_arg( ray_d_buff.ptr() );
         kern->set_arg( img_dim.ptr() );
         kern->set_arg( vis_image.ptr() );
         kern->set_arg( pre_image.ptr() );
         kern->set_arg( norm_image.ptr() );
         kern->set_arg( time_mem.ptr() );
         kern->set_arg( cl_output.ptr() );
         kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar16) );//local tree,
         kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar8) ); //local time tree
         kern->set_local_arg( local_threads[0]*local_threads[1]*10*sizeof(cl_uchar) ); //cumsum buffer, imindex buffer
         kern->execute(queue, 2, local_threads, global_threads);
         int status = clFinish(queue);
         if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
           return false;
         gpu_time += kern->exec_time();

         //clear render kernel args so it can reset em on next execution
         kern->clear_args();
         kern->release_current_event();
       }
       else if (i==UPDATE_CELL)
       {
         bocl_mem* num_obs   = opencl_cache->get_data(*id,num_obs_type,data_buffer_length*nobsTypeSize,false);


         bocl_mem *change   = opencl_cache->get_data<BSTM_CHANGE>(*id, num_time_trees*bstm_data_traits<BSTM_CHANGE>::datasize(),false);

         local_threads[0] = 64;
         local_threads[1] = 1 ;
         global_threads[0]= RoundUp(info_buffer_t->tree_buffer_length ,local_threads[0]);
         global_threads[1]=1;

         kern->set_arg( blk_t_info );
         kern->set_arg( blk_t );
         kern->set_arg( alpha );
         kern->set_arg( mog );
         kern->set_arg( num_obs );
         kern->set_arg( raydir );
         kern->set_arg( aux0 );
         kern->set_arg( aux1 );
         kern->set_arg( aux2 );
         kern->set_arg( aux3 );
         kern->set_arg( change );
         kern->set_arg( use_mask_mem.ptr() );
         kern->set_arg( mog_var_mem.ptr() );
         kern->set_arg( time_mem.ptr() );
         kern->set_arg( up_alpha_mem.ptr() );
         kern->set_arg( up_changes_mem .ptr() );

         kern->set_local_arg( local_threads[0]*local_threads[1]*sizeof(cl_uchar8) ); //local time tree

         //execute kernel
         kern->execute(queue, 2, local_threads, global_threads);
         int status = clFinish(queue);
         if (!check_val(status, MEM_FAILURE, "UPDATE EXECUTE FAILED: " + error_to_string(status)))
           return false;
         gpu_time += kern->exec_time();

         //clear render kernel args so it can reset em on next execution
         kern->clear_args();
         kern->release_current_event();

         //write info to disk
         alpha->read_to_buffer(queue);
         mog->read_to_buffer(queue);
         num_obs->read_to_buffer(queue);
       }

     }
   }



   delete [] vis_buff;
   delete [] pre_buff;
   delete [] norm_buff;
   delete [] input_buff;
   delete [] ray_origins;
   delete [] ray_directions;
   opencl_cache->unref_mem(in_image.ptr());
   opencl_cache->unref_mem(vis_image.ptr());
   opencl_cache->unref_mem(pre_image.ptr());
   opencl_cache->unref_mem(norm_image.ptr());
   opencl_cache->unref_mem(ray_o_buff.ptr());
   opencl_cache->unref_mem(ray_d_buff.ptr());

   vcl_cout<<"Gpu time "<<gpu_time<<" transfer time "<<transfer_time<<vcl_endl;
   clReleaseCommandQueue(queue);
   return true;

}


//Returns vector of color update kernels (and caches them per device
vcl_vector<bocl_kernel*>& bstm_ocl_update_viewdep::get_kernels(bocl_device_sptr device, vcl_string opts, bool isRGB)
{
  // compile kernels if not already compiled
  vcl_string identifier = device->device_identifier() + opts;
  if (kernels_.find(identifier) != kernels_.end())
    return kernels_[identifier];

  //otherwise compile the kernels
  vcl_cout<<"=== bstm_ocl_update_viewdep::compiling kernels on device "<<identifier<<"==="<<vcl_endl;

  vcl_vector<vcl_string> src_paths;
  vcl_string source_dir = vcl_string(VCL_SOURCE_ROOT_DIR) + "/contrib/brl/bseg/bstm/ocl/cl/";
  src_paths.push_back(source_dir + "scene_info.cl");
  src_paths.push_back(source_dir + "pixel_conversion.cl");
  src_paths.push_back(source_dir + "bit/bit_tree_library_functions.cl");
  src_paths.push_back(source_dir + "bit/time_tree_library_functions.cl");
  src_paths.push_back(source_dir + "backproject.cl");
  src_paths.push_back(source_dir + "statistics_library_functions.cl");
  src_paths.push_back(source_dir + "view_dep_app_helper_functions.cl");
  src_paths.push_back(source_dir + "bit/update_view_dep_app_kernels.cl");
  vcl_vector<vcl_string> non_ray_src = vcl_vector<vcl_string>(src_paths);

  //push ray trace files
  src_paths.push_back(source_dir + "view_dep_app_update_functors.cl");
  src_paths.push_back(source_dir + "bit/cast_ray_bit.cl");

  //compilation options
  vcl_string options = opts;

  //populate vector of kernels
  vcl_vector<bocl_kernel*> vec_kernels;

  //seg len pass
  bocl_kernel* seg_len = new bocl_kernel();
  vcl_string seg_opts = options + " -D SEGLEN -D STEP_CELL=step_cell_seglen(aux_args,data_ptr,data_ptr_tt,d)";
  seg_len->create_kernel(&device->context(), device->device_id(), src_paths, "seg_len_main", seg_opts, "update::seg_len");
  vec_kernels.push_back(seg_len);


  bocl_kernel* pre_inf = new bocl_kernel();
  vcl_string pre_opts = options + " -D PREINF -D STEP_CELL=step_cell_preinf(aux_args,data_ptr,data_ptr_tt,d)";
  pre_inf->create_kernel(&device->context(), device->device_id(), src_paths, "pre_inf_main", pre_opts, "update::pre_inf");
  vec_kernels.push_back(pre_inf);

  //may need DIFF LIST OF SOURCES FOR THIS GUY
  bocl_kernel* proc_img = new bocl_kernel();
  vcl_string proc_opts = options + " -D PROC_NORM ";
  proc_img->create_kernel(&device->context(), device->device_id(), non_ray_src, "proc_norm_image", proc_opts, "update::proc_norm_image");
  vec_kernels.push_back(proc_img);

  //push back cast_ray_bit
  bocl_kernel* bayes_main = new bocl_kernel();
  vcl_string bayes_opt = options + " -D BAYES -D STEP_CELL=step_cell_bayes(aux_args,data_ptr,data_ptr_tt,d)";
  bayes_main->create_kernel(&device->context(), device->device_id(), src_paths, "bayes_main", bayes_opt, "update::bayes_main");
  vec_kernels.push_back(bayes_main);

  bocl_kernel* update = new bocl_kernel();
  vcl_string update_opts = options + " -D UPDATE_BIT_SCENE_MAIN ";
  update->create_kernel(&device->context(), device->device_id(), non_ray_src, "update_bit_scene_main", update_opts, "update::update_main");
  vec_kernels.push_back(update);

  //store and return
  kernels_[identifier] = vec_kernels;
  return kernels_[identifier];
}


//makes sure appearance types correspond correctly
bool bstm_ocl_update_viewdep::validate_appearances(bstm_scene_sptr scene,
                                            vcl_string& data_type,
                                            int& appTypeSize,
                                            vcl_string& num_obs_type,
                                            vcl_string& options)
{
  vcl_vector<vcl_string> apps = scene->appearances();
  bool foundDataType = false, foundNumObsType = false;
  for (unsigned int i=0; i<apps.size(); ++i) {
    if ( apps[i] == bstm_data_traits<BSTM_MOG6_VIEW>::prefix() )
    {
      data_type = apps[i];
      foundDataType = true;
      options+= " -D MOG_VIEW_DEP ";
      appTypeSize = (int)bstm_data_info::datasize(bstm_data_traits<BSTM_MOG6_VIEW>::prefix());
    }
    else if ( apps[i] == bstm_data_traits<BSTM_MOG6_VIEW_COMPACT>::prefix() )
    {
      data_type = apps[i];
      foundDataType = true;
      options+=" -D MOG_VIEW_DEP_COMPACT ";
      appTypeSize = (int)bstm_data_info::datasize(bstm_data_traits<BSTM_MOG6_VIEW_COMPACT>::prefix());
    }

    else if( apps[i] == bstm_data_traits<BSTM_NUM_OBS_VIEW>::prefix())
    {
      num_obs_type = apps[i];
      foundNumObsType = true;
    }
    else if( apps[i] == bstm_data_traits<BSTM_NUM_OBS_VIEW_COMPACT>::prefix())
    {
      num_obs_type = apps[i];
      options+= " -D NUM_OBS_VIEW_COMPACT ";
      foundNumObsType = true;
    }
  }
  if (!foundDataType) {
    vcl_cout<<"BSTM_OPENCL_VIEW_BASED_UPDATE_PROCESS ERROR: scene doesn't have BSTM_MOG6_VIEW or BSTM_MOG6_VIEW_COMPACT data type"<<vcl_endl;
    return false;
  }
  if (!foundNumObsType) {
    vcl_cout<<"BSTM_OPENCL_VIEW_BASED_UPDATE_PROCESS ERROR: scene doesn't have BSTM_NUM_OBS_VIEW or BSTM_NUM_OBS_VIEW_COMPACT type"<<vcl_endl;
    return false;
  }
  return true;
}
