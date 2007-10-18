#include "bwm_soview2D_cross.h"

void bwm_soview2D_cross::draw() const
{
#ifdef DEBUG
  vcl_cerr << "bwm_soview2D_cross::draw() line id=" << id << '\n';
#endif

  glBegin(GL_LINES);
  // horizantal lines of cross
  glVertex2f(x-r_,y);
  glVertex2f(x-0.5,y);
  glVertex2f(x+0.5,y);
  glVertex2f(x+r_,y);

  // vertical lines of cross
  glVertex2f(x,y-r_);
  glVertex2f(x,y-0.5);
  glVertex2f(x,y+0.5);
  glVertex2f(x,y+r_);
  glEnd();
}
