#include "bwm_observer_mgr.h"


bwm_observer_cam* bwm_observer_mgr::BWM_MASTER_OBSERVER = 0;

bwm_observer_mgr* bwm_observer_mgr::instance_ = 0;

bwm_observer_mgr* bwm_observer_mgr::instance() {
  if (!instance_) 
    instance_ = new bwm_observer_mgr();
  return bwm_observer_mgr::instance_;
   
}

vcl_vector<bwm_observer_cam_rat*> bwm_observer_mgr::observers_cam_rat()
{
  vcl_vector<bwm_observer_cam_rat*> v;

  for (unsigned i=0; i< observers_.size(); i++) {
    if (observers_[i]->type_name().compare("bwm_observer_cam_rat") == 0)
      v.push_back(static_cast<bwm_observer_cam_rat*> (observers_[i]));
  }

  return v;
}

void bwm_observer_mgr::attach(bwm_observable_sptr obs)
{
  for (unsigned i=0; i<observers_.size(); i++) 
    obs->attach(observers_[i]);
}

void bwm_observer_mgr::remove(bwm_observer* observer)
{
  for (unsigned i=0; i<observers_.size(); i++) 
    if (observers_[i] == observer)
      observers_.erase(observers_.begin()+i);
}

void bwm_observer_mgr::collect_corr()
{
  //vcl_vector<vcl_pair vgl_homg_point_2d<double> > corr_pts;
  bwm_corr_sptr corr = new bwm_corr();
  bool found = false;
  vgl_point_2d<double> pt;

  // set mode 
  if (corr_mode_ == IMAGE_TO_IMAGE)
    corr->set_mode(true);
  else if (corr_mode_ == WORLD_TO_IMAGE) {
    corr->set_mode(false);
    corr->set_world_pt(corr_world_pt_);
  } 
  else
    vcl_cerr << "Unknown correspondence mode!" << vcl_endl;

  for(unsigned i=0; i< observers_.size(); i++) {
    if ((observers_[i]->type_name().compare("bwm_observer_cam_rat") == 0) ||
        (observers_[i]->type_name().compare("bwm_observer_cam_proj") == 0)) {
      bwm_observer_cam* obs = static_cast<bwm_observer_cam *> (observers_[i]);
      if (obs->corr_pt(pt)) {
        corr->set_match(obs, pt.x(), pt.y());
        found = true;
      }
    }
  }
  
  if (found)
    corr_list_.push_back(corr);
  else
    vcl_cerr << "No Correspondence SET yet!!" << vcl_endl;
}

void bwm_observer_mgr::update_corr(bwm_observer_cam* obs,
                                   vgl_point_2d<double> old_pt,
                                   vgl_point_2d<double> new_pt)
{
  for(unsigned i=0; i< corr_list_.size(); i++) {
    bwm_corr_sptr corr = corr_list_[i];
    if (corr->update_match(obs, old_pt, new_pt))
      return;
  }
}

void bwm_observer_mgr::save_corr(vcl_ostream& s)
{
  if (corr_list_.size() == 0)
    vcl_cerr << "No correspondences to save yet! " << vcl_endl;
  else {
    s << "Cameras:" << vcl_endl;
    // first write down the camera info
    vcl_map<bwm_observer_cam*, unsigned> camera_map; 
    for(unsigned i=0; i< observers_.size(); i++) {
      if ((observers_[i]->type_name().compare("bwm_observer_cam_rat") == 0) ||
          (observers_[i]->type_name().compare("bwm_observer_cam_proj") == 0)) {
        bwm_observer_cam* obs = static_cast<bwm_observer_cam *> (observers_[i]);
        s << "Camera [" << i << "] : " << obs->camera_path() << vcl_endl;
        camera_map[obs] = i;
      }
    }

    for(unsigned i=0; i< corr_list_.size(); i++) {
      bwm_corr_sptr corr = corr_list_[i];
      s << "\n Correspondence " << i << ":" << vcl_endl;

      vcl_vector<bwm_observer_cam*> obs = corr->observers();
      if (corr->mode() == false) { // WORLD TO IMAGE
        s << "WORLD POINT: [" << corr->world_pt().x() << "," << corr->world_pt().y() 
          << "," << corr->world_pt().z() << "]" << vcl_endl;
      }
      for(unsigned j=0; j< obs.size(); j++) {
        vgl_point_2d<double> p;
        if (corr->match(obs[j], p))
          s << camera_map[obs[j]] << " " << p.x() << " " << p.y() << vcl_endl;
      }
      //s << *corr_list_[i] ;
    }
  }
}

void bwm_observer_mgr::delete_last_corr()
{
  unsigned i = corr_list_.size();
  if (i > 0) {
    // first notify the observer to delete the corr point on the screen
    bwm_corr_sptr corr = corr_list_[i-1];
    vcl_vector<bwm_observer_cam*> obs = corr->observers();
    for (unsigned i=0; i<obs.size(); i++) {
      obs[i]->remove_corr_pt();
      obs[i]->post_redraw();
    }
    corr_list_.pop_back();  // removes the last element
  }
}

void bwm_observer_mgr::delete_all_corr()
{
  delete_last_corr();
  corr_list_.resize(0);
}

void bwm_observer_mgr::print_observers()
{
  for (unsigned i=0; i< observers_.size(); i++) {
    vcl_cout <<  i << " - " << observers_[i]->type_name() << vcl_endl;
  }
}
